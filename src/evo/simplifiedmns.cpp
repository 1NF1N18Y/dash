// Copyright (c) 2017-2022 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <evo/simplifiedmns.h>

#include <evo/cbtx.h>
#include <core_io.h>
#include <evo/deterministicmns.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/quorums.h>
#include <llmq/utils.h>
#include <evo/specialtx.h>

#include <pubkey.h>
#include <serialize.h>
#include <version.h>

#include <base58.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <univalue.h>
#include <validation.h>
#include <key_io.h>
#include <util/underlying.h>
#include <util/enumerate.h>

CSimplifiedMNListEntry::CSimplifiedMNListEntry(const CDeterministicMN& dmn) :
    proRegTxHash(dmn.proTxHash),
    confirmedHash(dmn.pdmnState->confirmedHash),
    service(dmn.pdmnState->addr),
    pubKeyOperator(dmn.pdmnState->pubKeyOperator),
    keyIDVoting(dmn.pdmnState->keyIDVoting),
    isValid(!dmn.pdmnState->IsBanned()),
    scriptPayout(dmn.pdmnState->scriptPayout),
    scriptOperatorPayout(dmn.pdmnState->scriptOperatorPayout),
    nVersion(dmn.pdmnState->nVersion == CProRegTx::LEGACY_BLS_VERSION ? LEGACY_BLS_VERSION : BASIC_BLS_VERSION),
    nType(dmn.nType),
    platformHTTPPort(dmn.pdmnState->platformHTTPPort),
    platformNodeID(dmn.pdmnState->platformNodeID)
{
}

uint256 CSimplifiedMNListEntry::CalcHash() const
{
    CHashWriter hw(SER_GETHASH, CLIENT_VERSION);
    hw << *this;
    return hw.GetHash();
}

std::string CSimplifiedMNListEntry::ToString() const
{
    CTxDestination dest;
    std::string payoutAddress = "unknown";
    std::string operatorPayoutAddress = "none";
    if (ExtractDestination(scriptPayout, dest)) {
        payoutAddress = EncodeDestination(dest);
    }
    if (ExtractDestination(scriptOperatorPayout, dest)) {
        operatorPayoutAddress = EncodeDestination(dest);
    }

    return strprintf("CSimplifiedMNListEntry(nVersion=%d, nType=%d, proRegTxHash=%s, confirmedHash=%s, service=%s, pubKeyOperator=%s, votingAddress=%s, isValid=%d, payoutAddress=%s, operatorPayoutAddress=%s, platformHTTPPort=%d, platformNodeID=%s)",
                     nVersion, ToUnderlying(nType), proRegTxHash.ToString(), confirmedHash.ToString(), service.ToString(false), pubKeyOperator.ToString(), EncodeDestination(PKHash(keyIDVoting)), isValid, payoutAddress, operatorPayoutAddress, platformHTTPPort, platformNodeID.ToString());
}

void CSimplifiedMNListEntry::ToJson(UniValue& obj, bool extended) const
{
    obj.clear();
    obj.setObject();
    obj.pushKV("nVersion", nVersion);
    obj.pushKV("nType", ToUnderlying(nType));
    obj.pushKV("proRegTxHash", proRegTxHash.ToString());
    obj.pushKV("confirmedHash", confirmedHash.ToString());
    obj.pushKV("service", service.ToString(false));
    obj.pushKV("pubKeyOperator", pubKeyOperator.ToString());
    obj.pushKV("votingAddress", EncodeDestination(PKHash(keyIDVoting)));
    obj.pushKV("isValid", isValid);
    if (nType == MnType::HighPerformance) {
        obj.pushKV("platformHTTPPort", platformHTTPPort);
        obj.pushKV("platformNodeID", platformNodeID.ToString());
    }

    if (!extended) return;

    CTxDestination dest;
    if (ExtractDestination(scriptPayout, dest)) {
        obj.pushKV("payoutAddress", EncodeDestination(dest));
    }
    if (ExtractDestination(scriptOperatorPayout, dest)) {
        obj.pushKV("operatorPayoutAddress", EncodeDestination(dest));
    }
}

// TODO: Invistigate if we can delete this constructor
CSimplifiedMNList::CSimplifiedMNList(const std::vector<CSimplifiedMNListEntry>& smlEntries)
{
    mnList.resize(smlEntries.size());
    for (size_t i = 0; i < smlEntries.size(); i++) {
        mnList[i] = std::make_unique<CSimplifiedMNListEntry>(smlEntries[i]);
    }

    std::sort(mnList.begin(), mnList.end(), [&](const std::unique_ptr<CSimplifiedMNListEntry>& a, const std::unique_ptr<CSimplifiedMNListEntry>& b) {
        return a->proRegTxHash.Compare(b->proRegTxHash) < 0;
    });
}

CSimplifiedMNList::CSimplifiedMNList(const CDeterministicMNList& dmnList)
{
    mnList.resize(dmnList.GetAllMNsCount());

    size_t i = 0;
    dmnList.ForEachMN(false, [this, &i](auto& dmn) {
        mnList[i++] = std::make_unique<CSimplifiedMNListEntry>(dmn);
    });

    std::sort(mnList.begin(), mnList.end(), [&](const std::unique_ptr<CSimplifiedMNListEntry>& a, const std::unique_ptr<CSimplifiedMNListEntry>& b) {
        return a->proRegTxHash.Compare(b->proRegTxHash) < 0;
    });
}

uint256 CSimplifiedMNList::CalcMerkleRoot(bool* pmutated) const
{
    std::vector<uint256> leaves;
    leaves.reserve(mnList.size());
    for (const auto& e : mnList) {
        leaves.emplace_back(e->CalcHash());
    }
    return ComputeMerkleRoot(leaves, pmutated);
}

bool CSimplifiedMNList::operator==(const CSimplifiedMNList& rhs) const
{
    return mnList.size() == rhs.mnList.size() &&
            std::equal(mnList.begin(), mnList.end(), rhs.mnList.begin(),
                [](const std::unique_ptr<CSimplifiedMNListEntry>& left, const std::unique_ptr<CSimplifiedMNListEntry>& right)
                {
                    return *left == *right;
                }
            );
}

CSimplifiedMNListDiff::CSimplifiedMNListDiff() = default;

CSimplifiedMNListDiff::~CSimplifiedMNListDiff() = default;

bool CSimplifiedMNListDiff::BuildQuorumsDiff(const CBlockIndex* baseBlockIndex, const CBlockIndex* blockIndex,
                                             const llmq::CQuorumBlockProcessor& quorum_block_processor)
{
    auto baseQuorums = quorum_block_processor.GetMinedAndActiveCommitmentsUntilBlock(baseBlockIndex);
    auto quorums = quorum_block_processor.GetMinedAndActiveCommitmentsUntilBlock(blockIndex);

    std::set<std::pair<Consensus::LLMQType, uint256>> baseQuorumHashes;
    std::set<std::pair<Consensus::LLMQType, uint256>> quorumHashes;
    for (const auto& [llmqType, vecBlockIndex] : baseQuorums) {
        for (const auto& blockindex : vecBlockIndex) {
            baseQuorumHashes.emplace(llmqType, blockindex->GetBlockHash());
        }
    }
    for (const auto& [llmqType, vecBlockIndex] : quorums) {
        for (const auto& blockindex : vecBlockIndex) {
            quorumHashes.emplace(llmqType, blockindex->GetBlockHash());
        }
    }

    for (const auto& p : baseQuorumHashes) {
        if (!quorumHashes.count(p)) {
            deletedQuorums.emplace_back((uint8_t)p.first, p.second);
        }
    }
    for (const auto& p : quorumHashes) {
        const auto& [llmqType, hash] = p;
        if (!baseQuorumHashes.count(p)) {
            uint256 minedBlockHash;
            llmq::CFinalCommitmentPtr qc = quorum_block_processor.GetMinedCommitment(llmqType, hash, minedBlockHash);
            if (qc == nullptr) {
                return false;
            }
            newQuorums.emplace_back(*qc);
        }
    }

    return true;
}

bool CSimplifiedMNListDiff::BuildQuorumChainlockInfo(const CBlockIndex* blockIndex)
{
    // Group quorums (indexes corresponding to entries of newQuorums) per CBlockIndex containing the expected CL signature in CbTx.
    // We want to avoid to load CbTx now, as more than one quorum will target the same block: hence we want to load CbTxs once per block (heavy operation).
    std::multimap<const CBlockIndex*, uint16_t>  workBaseBlockIndexMap;

    for (const auto [idx, e] : enumerate(newQuorums)) {
        auto quorum = llmq::quorumManager->GetQuorum(e.llmqType, e.quorumHash);
        // In case of rotation, all rotated quorums rely on the CL sig expected in the cycleBlock (the block of the first DKG) - 8
        // In case of non-rotation, quorums rely on the CL sig expected in the block of the DKG - 8
        const CBlockIndex* pWorkBaseBlockIndex =
                blockIndex->GetAncestor(quorum->m_quorum_base_block_index->nHeight - quorum->qc->quorumIndex - 8);

        workBaseBlockIndexMap.insert(std::make_pair(pWorkBaseBlockIndex, idx));
    }

    for(auto it = workBaseBlockIndexMap.begin(); it != workBaseBlockIndexMap.end(); ) {
        // Process each key (CBlockIndex containing the expected CL signature in CbTx) of the std::multimap once
        const CBlockIndex* pWorkBaseBlockIndex = it->first;
        const auto cbcl = GetNonNullCoinbaseChainlock(pWorkBaseBlockIndex);
        CBLSSignature sig;
        if (cbcl.has_value()) {
            sig = cbcl.value().first;
        }
        // Get the range of indexes (values) for the current key and merge them into a single std::set
        const auto [begin, end] = workBaseBlockIndexMap.equal_range(it->first);
        std::set<uint16_t> idx_set;
        std::transform(begin, end, std::inserter(idx_set, idx_set.end()), [](const auto& pair) { return pair.second; });
        // Advance the iterator to the next key
        it = end;

        // Different CBlockIndex can contain the same CL sig in CbTx (both non-null or null during the first blocks after v20 activation)
        // Hence, we need to merge the std::set if another std::set already exists for the same sig.
        if (auto [it_sig, inserted] = quorumsCLSigs.insert({sig, idx_set}); !inserted) {
            it_sig->second.insert(idx_set.begin(), idx_set.end());
        }
    }

    return true;
}

void CSimplifiedMNListDiff::ToJson(UniValue& obj, bool extended) const
{
    obj.setObject();

    obj.pushKV("nVersion", nVersion);
    obj.pushKV("baseBlockHash", baseBlockHash.ToString());
    obj.pushKV("blockHash", blockHash.ToString());

    CDataStream ssCbTxMerkleTree(SER_NETWORK, PROTOCOL_VERSION);
    ssCbTxMerkleTree << cbTxMerkleTree;
    obj.pushKV("cbTxMerkleTree", HexStr(ssCbTxMerkleTree));

    obj.pushKV("cbTx", EncodeHexTx(*cbTx));

    UniValue deletedMNsArr(UniValue::VARR);
    for (const auto& h : deletedMNs) {
        deletedMNsArr.push_back(h.ToString());
    }
    obj.pushKV("deletedMNs", deletedMNsArr);

    UniValue mnListArr(UniValue::VARR);
    for (const auto& e : mnList) {
        UniValue eObj;
        e.ToJson(eObj, extended);
        mnListArr.push_back(eObj);
    }
    obj.pushKV("mnList", mnListArr);

    UniValue deletedQuorumsArr(UniValue::VARR);
    for (const auto& e : deletedQuorums) {
        UniValue eObj(UniValue::VOBJ);
        eObj.pushKV("llmqType", e.first);
        eObj.pushKV("quorumHash", e.second.ToString());
        deletedQuorumsArr.push_back(eObj);
    }
    obj.pushKV("deletedQuorums", deletedQuorumsArr);

    UniValue newQuorumsArr(UniValue::VARR);
    for (const auto& e : newQuorums) {
        UniValue eObj;
        e.ToJson(eObj);
        newQuorumsArr.push_back(eObj);
    }
    obj.pushKV("newQuorums", newQuorumsArr);

    CCbTx cbTxPayload;
    if (GetTxPayload(*cbTx, cbTxPayload)) {
        obj.pushKV("merkleRootMNList", cbTxPayload.merkleRootMNList.ToString());
        if (cbTxPayload.nVersion >= 2) {
            obj.pushKV("merkleRootQuorums", cbTxPayload.merkleRootQuorums.ToString());
        }
    }

    UniValue quorumsCLSigsArr(UniValue::VARR);
    for (const auto& [signature, quorumsIndexes] : quorumsCLSigs) {
        UniValue j(UniValue::VOBJ);
        UniValue idxArr(UniValue::VARR);
        for (const auto& idx : quorumsIndexes) {
            idxArr.push_back(idx);
        }
        j.pushKV(signature.ToString(),idxArr);
        quorumsCLSigsArr.push_back(j);
    }
    obj.pushKV("quorumsCLSigs", quorumsCLSigsArr);
}

CSimplifiedMNListDiff BuildSimplifiedDiff(const CDeterministicMNList& from, const CDeterministicMNList& to, bool extended)
{
    CSimplifiedMNListDiff diffRet;
    diffRet.baseBlockHash = from.GetBlockHash();
    diffRet.blockHash = to.GetBlockHash();

    to.ForEachMN(false, [&](const auto& toPtr) {
        auto fromPtr = from.GetMN(toPtr.proTxHash);
        if (fromPtr == nullptr) {
            CSimplifiedMNListEntry sme(toPtr);
            diffRet.mnList.push_back(std::move(sme));
        } else {
            CSimplifiedMNListEntry sme1(toPtr);
            CSimplifiedMNListEntry sme2(*fromPtr);
            if ((sme1 != sme2) ||
                (extended && (sme1.scriptPayout != sme2.scriptPayout || sme1.scriptOperatorPayout != sme2.scriptOperatorPayout))) {
                    diffRet.mnList.push_back(std::move(sme1));
            }
        }
    });

    from.ForEachMN(false, [&](auto& fromPtr) {
        auto toPtr = to.GetMN(fromPtr.proTxHash);
        if (toPtr == nullptr) {
            diffRet.deletedMNs.emplace_back(fromPtr.proTxHash);
        }
    });

    return diffRet;
}

bool BuildSimplifiedMNListDiff(const uint256& baseBlockHash, const uint256& blockHash, CSimplifiedMNListDiff& mnListDiffRet,
                               const llmq::CQuorumBlockProcessor& quorum_block_processor, std::string& errorRet, bool extended)
{
    AssertLockHeld(cs_main);
    mnListDiffRet = CSimplifiedMNListDiff();

    const CBlockIndex* baseBlockIndex = ::ChainActive().Genesis();
    if (!baseBlockHash.IsNull()) {
        baseBlockIndex = g_chainman.m_blockman.LookupBlockIndex(baseBlockHash);
        if (!baseBlockIndex) {
            errorRet = strprintf("block %s not found", baseBlockHash.ToString());
            return false;
        }
    }

    const CBlockIndex* blockIndex = g_chainman.m_blockman.LookupBlockIndex(blockHash);
    if (!blockIndex) {
        errorRet = strprintf("block %s not found", blockHash.ToString());
        return false;
    }

    if (!::ChainActive().Contains(baseBlockIndex) || !::ChainActive().Contains(blockIndex)) {
        errorRet = strprintf("block %s and %s are not in the same chain", baseBlockHash.ToString(), blockHash.ToString());
        return false;
    }
    if (baseBlockIndex->nHeight > blockIndex->nHeight) {
        errorRet = strprintf("base block %s is higher then block %s", baseBlockHash.ToString(), blockHash.ToString());
        return false;
    }

    auto baseDmnList = deterministicMNManager->GetListForBlock(baseBlockIndex);
    auto dmnList = deterministicMNManager->GetListForBlock(blockIndex);
    mnListDiffRet = BuildSimplifiedDiff(baseDmnList, dmnList, extended);

    // We need to return the value that was provided by the other peer as it otherwise won't be able to recognize the
    // response. This will usually be identical to the block found in baseBlockIndex. The only difference is when a
    // null block hash was provided to get the diff from the genesis block.
    mnListDiffRet.baseBlockHash = baseBlockHash;

    if (!mnListDiffRet.BuildQuorumsDiff(baseBlockIndex, blockIndex, quorum_block_processor)) {
        errorRet = strprintf("failed to build quorums diff");
        return false;
    }

    if (llmq::utils::IsV20Active(blockIndex)) {
        if (!mnListDiffRet.BuildQuorumChainlockInfo(blockIndex)) {
            errorRet = strprintf("failed to build quorums chainlocks info");
            return false;
        }
    }

    // TODO store coinbase TX in CBlockIndex
    CBlock block;
    if (!ReadBlockFromDisk(block, blockIndex, Params().GetConsensus())) {
        errorRet = strprintf("failed to read block %s from disk", blockHash.ToString());
        return false;
    }

    mnListDiffRet.cbTx = block.vtx[0];

    std::vector<uint256> vHashes;
    std::vector<bool> vMatch(block.vtx.size(), false);
    for (const auto& tx : block.vtx) {
        vHashes.emplace_back(tx->GetHash());
    }
    vMatch[0] = true; // only coinbase matches
    mnListDiffRet.cbTxMerkleTree = CPartialMerkleTree(vHashes, vMatch);

    return true;
}
