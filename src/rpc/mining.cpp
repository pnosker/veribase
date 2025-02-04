// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <amount.h>
#include <chain.h>
#include <chainparams.h>
#include <consensus/consensus.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <key_io.h>
#include <miner.h>
#include <net.h>
#include <node/context.h>
#include <pow.h>
#include <pos.h>
#include <rpc/blockchain.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <script/descriptor.h>
#include <script/script.h>
#include <script/signingprovider.h>
#include <shutdown.h>
#include <txmempool.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/system.h>
#include <validation.h>
#include <validationinterface.h>
#include <warnings.h>
#include <wallet/rpcwallet.h> // Probably need to avoid that ...

#include <memory>
#include <stdint.h>

static UniValue generateBlocks(const CTxMemPool& mempool, const CScript& coinbase_script, int nGenerate, uint64_t nMaxTries)
{
    int nHeightEnd = 0;
    int nHeight = 0;

    {   // Don't keep cs_main locked
        LOCK(cs_main);
        nHeight = ::ChainActive().Height();
        nHeightEnd = nHeight+nGenerate;
    }
    unsigned int nExtraNonce = 0;
    UniValue blockHashes(UniValue::VARR);
    while (nHeight < nHeightEnd && !ShutdownRequested())
    {
        std::unique_ptr<CBlockTemplate> pblocktemplate(BlockAssembler(mempool, Params()).CreateNewBlock(coinbase_script));
        if (!pblocktemplate.get())
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Couldn't create new block");
        CBlock *pblock = &pblocktemplate->block;
        {
            LOCK(cs_main);
            IncrementExtraNonce(pblock, ::ChainActive().Tip(), nExtraNonce);
        }
        while (nMaxTries > 0 && pblock->nNonce < std::numeric_limits<uint32_t>::max() && !CheckProofOfWork(pblock->GetHash(), pblock->nBits, Params().GetConsensus()) && !ShutdownRequested()) {
            ++pblock->nNonce;
            --nMaxTries;
        }
        if (nMaxTries == 0 || ShutdownRequested()) {
            break;
        }
        if (pblock->nNonce == std::numeric_limits<uint32_t>::max()) {
            continue;
        }
        std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(*pblock);
        if (!ProcessNewBlock(Params(), shared_pblock, true, nullptr))
            throw JSONRPCError(RPC_INTERNAL_ERROR, "ProcessNewBlock, block not accepted");
        ++nHeight;
        blockHashes.push_back(pblock->GetHash().GetHex());
    }
    return blockHashes;
}

static UniValue generatetodescriptor(const JSONRPCRequest& request)
{
    RPCHelpMan{
        "generatetodescriptor",
        "\nMine blocks immediately to a specified descriptor (before the RPC call returns)\n",
        {
            {"num_blocks", RPCArg::Type::NUM, RPCArg::Optional::NO, "How many blocks are generated immediately."},
            {"descriptor", RPCArg::Type::STR, RPCArg::Optional::NO, "The descriptor to send the newly generated bitcoin to."},
            {"maxtries", RPCArg::Type::NUM, /* default */ "1000000", "How many iterations to try."},
        },
        RPCResult{
            RPCResult::Type::ARR, "", "hashes of blocks generated",
            {
                {RPCResult::Type::STR_HEX, "", "blockhash"},
            }
        },
        RPCExamples{
            "\nGenerate 11 blocks to mydesc\n" + HelpExampleCli("generatetodescriptor", "11 \"mydesc\"")},
    }
        .Check(request);

    const int num_blocks{request.params[0].get_int()};
    const int64_t max_tries{request.params[2].isNull() ? 1000000 : request.params[2].get_int()};

    FlatSigningProvider key_provider;
    std::string error;
    const auto desc = Parse(request.params[1].get_str(), key_provider, error, /* require_checksum = */ false);
    if (!desc) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, error);
    }
    if (desc->IsRange()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Ranged descriptor not accepted. Maybe pass through deriveaddresses first?");
    }

    FlatSigningProvider provider;
    std::vector<CScript> coinbase_script;
    if (!desc->Expand(0, key_provider, coinbase_script, provider)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Cannot derive script without private keys"));
    }

    const CTxMemPool& mempool = EnsureMemPool();

    CHECK_NONFATAL(coinbase_script.size() == 1);

    return generateBlocks(mempool, coinbase_script.at(0), num_blocks, max_tries);
}

static UniValue generatetoaddress(const JSONRPCRequest& request)
{
            RPCHelpMan{"generatetoaddress",
                "\nMine blocks immediately to a specified address (before the RPC call returns)\n",
                {
                    {"nblocks", RPCArg::Type::NUM, RPCArg::Optional::NO, "How many blocks are generated immediately."},
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The address to send the newly generated bitcoin to."},
                    {"maxtries", RPCArg::Type::NUM, /* default */ "1000000", "How many iterations to try."},
                },
                RPCResult{
                    RPCResult::Type::ARR, "", "hashes of blocks generated",
                    {
                        {RPCResult::Type::STR_HEX, "", "blockhash"},
                    }},
                RPCExamples{
            "\nGenerate 11 blocks to myaddress\n"
            + HelpExampleCli("generatetoaddress", "11 \"myaddress\"")
            + "If you are running the bitcoin core wallet, you can get a new address to send the newly generated bitcoin to with:\n"
            + HelpExampleCli("getnewaddress", "")
                },
            }.Check(request);

    int nGenerate = request.params[0].get_int();
    uint64_t nMaxTries = 1000000;
    if (!request.params[2].isNull()) {
        nMaxTries = request.params[2].get_int();
    }

    CTxDestination destination = DecodeDestination(request.params[1].get_str());
    if (!IsValidDestination(destination)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Error: Invalid address");
    }

    const CTxMemPool& mempool = EnsureMemPool();

    CScript coinbase_script = GetScriptForDestination(destination);

    return generateBlocks(mempool, coinbase_script, nGenerate, nMaxTries);
}

static UniValue getmininginfo(const JSONRPCRequest& request)
{
    if( ! Params().IsVericoin()) {

        RPCHelpMan{"getmininginfo",
            "\nReturns a json object containing mining-related information.",
            {},
            RPCResult{
                RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::NUM, "blockreward", "The current block reward"},
                    {RPCResult::Type::NUM, "blocks", "The current block"},
                    {RPCResult::Type::NUM, "blocksperhour", "Number of blocks per hour"},
                    {RPCResult::Type::NUM, "blocktime", "Current time between blocks in minute"},
                    {RPCResult::Type::NUM, "currentblockweight", /* optional */ true, "The block weight of the last assembled block (only present if a block was ever assembled)"},
                    {RPCResult::Type::NUM, "currentblocktx", /* optional */ true, "The number of block transactions of the last assembled block (only present if a block was ever assembled)"},
                    {RPCResult::Type::NUM, "difficulty", "The current difficulty"},
                    {RPCResult::Type::NUM, "estimateblockrate", "Estimated block rate of your miner in hours"},
                    {RPCResult::Type::NUM, "hashrate", "Your miner hashrate in H/m"},
                    {RPCResult::Type::NUM, "networkhashps", "The network hashes per second"},
                    {RPCResult::Type::NUM, "pooledtx", "The size of the mempool"},
                    {RPCResult::Type::STR, "chain", "current network name (verium, vericoin)"},
                    {RPCResult::Type::STR, "warnings", "any network and blockchain warnings"},
                }
            },
            RPCExamples{
                HelpExampleCli("getmininginfo", "")
              + HelpExampleRpc("getmininginfo", "")
            },
        }.Check(request);

    }
    else {
        // for vericoin
        RPCHelpMan{"getmininginfo",
            "\nReturns a json object containing mining-related information.",
            {},
            RPCResult{
                RPCResult::Type::OBJ, "", "",
                {
                    {RPCResult::Type::NUM, "blockreward", "Proof of work block reward"},
                    {RPCResult::Type::NUM, "blocks", "The current block"},
                    {RPCResult::Type::NUM, "blocksperhour", "Number of blocks per hour"},
                    {RPCResult::Type::NUM, "currentblockweight", /* optional */ true, "The block weight of the last assembled block (only present if a block was ever assembled)"},
                    {RPCResult::Type::NUM, "currentblocktx", /* optional */ true, "The number of block transactions of the last assembled block (only present if a block was ever assembled)"},
                    {RPCResult::Type::OBJ, "difficulty", "The current difficulty", {
                            {RPCResult::Type::NUM, "proof-of-stake", "Proof Of Stake difficulty"},
                            {RPCResult::Type::NUM, "proof-of-work", "Proof Of Work difficulty"},
                            {RPCResult::Type::NUM, "search-interval", "The search interval"},
                        },
                    },
                    {RPCResult::Type::OBJ, "stakeweight", "Stake Weight", {
                            {RPCResult::Type::NUM, "combined", "Combined stake weight"},
                        },
                    },
                    {RPCResult::Type::NUM, "stakeinterest", "The current Staking intereset"},
                    {RPCResult::Type::NUM, "stakeinflation", "The current staking inflation"},
                    {RPCResult::Type::NUM, "networkhashps", "The network hashes per second"},
                    {RPCResult::Type::NUM, "networkstakeweight", "The network average stake weight"},
                    {RPCResult::Type::NUM, "pooledtx", "The size of the mempool"},
                    {RPCResult::Type::STR, "chain", "current network name (verium, vericoin)"},
                    {RPCResult::Type::STR, "warnings", "any network and blockchain warnings"},
                }
            },
            RPCExamples{
                HelpExampleCli("getmininginfo", "")
              + HelpExampleRpc("getmininginfo", "")
            },
        }.Check(request);
    }

    LOCK(cs_main);
    const CTxMemPool& mempool = EnsureMemPool();

    double nethashrate = GetPoWKHashPM();

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("blocks",           (int)::ChainActive().Height());
    if (BlockAssembler::m_last_block_weight) obj.pushKV("currentblockweight", *BlockAssembler::m_last_block_weight);
    if (BlockAssembler::m_last_block_num_txs) obj.pushKV("currentblocktx", *BlockAssembler::m_last_block_num_txs);
    obj.pushKV("networkhashps",    nethashrate/60);
    obj.pushKV("pooledtx",         (uint64_t)mempool.size());
    obj.pushKV("chain",            Params().NetworkIDString());
    obj.pushKV("warnings",         GetWarnings(false));

    obj.pushKV("blockreward",       (double)GetProofOfWorkReward(0,::ChainActive().Tip()->pprev)/COIN);
    obj.pushKV("blocksperhour",     GetBlockRatePerHour());

    if( ! Params().IsVericoin())
    {
        double blocktime = (double)CalculateBlocktime(::ChainActive().Tip())/60;
        double totalhashrate = hashrate;
        double minerate;
        if (totalhashrate == 0.0){minerate = 0.0;}
        else{
            minerate = 16.666667*(nethashrate*blocktime)/(totalhashrate);
        }

        obj.pushKV("blocktime",         blocktime);
        obj.pushKV("difficulty",       (double)GetDifficulty(::ChainActive().Tip()));
        obj.pushKV("estimateblockrate", minerate);
        obj.pushKV("hashrate",          totalhashrate);
    }
    else
    {
        std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);
        CWallet* const pwallet = wallet.get();

        uint64_t nWeight = 0;
        pwallet->GetStakeWeight(nWeight);
        double averageStakeWeight = GetAverageStakeWeight(::ChainActive().Tip()->pprev);

        UniValue difficulty(UniValue::VOBJ);
        difficulty.pushKV("proof-of-work",        GetDifficulty(NULL));
        difficulty.pushKV("proof-of-stake",       GetDifficulty(GetLastBlockIndex(::ChainActive().Tip(), true)));
        difficulty.pushKV("search-interval", (int)nLastCoinStakeSearchInterval);

        UniValue stakeweight(UniValue::VOBJ);
        stakeweight.pushKV("combined", nWeight);

        obj.pushKV("difficulty",     difficulty);
        obj.pushKV("stakeweight",    stakeweight);
        obj.pushKV("stakeinterest",  GetCurrentInterestRate(::ChainActive().Tip(), Params().GetConsensus()));
        obj.pushKV("stakeinflation", GetCurrentInflationRate(averageStakeWeight));
        obj.pushKV("netstakeweight", averageStakeWeight);
    }

    return obj;
}


// NOTE: Unlike wallet RPC (which use BTC values), mining RPCs follow GBT (BIP 22) in using satoshi amounts
static UniValue prioritisetransaction(const JSONRPCRequest& request)
{
            RPCHelpMan{"prioritisetransaction",
                "Accepts the transaction into mined blocks at a higher (or lower) priority\n",
                {
                    {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction id."},
                    {"dummy", RPCArg::Type::NUM, RPCArg::Optional::OMITTED_NAMED_ARG, "API-Compatibility for previous API. Must be zero or null.\n"
            "                  DEPRECATED. For forward compatibility use named arguments and omit this parameter."},
                    {"fee_delta", RPCArg::Type::NUM, RPCArg::Optional::NO, "The fee value (in satoshis) to add (or subtract, if negative).\n"
            "                  Note, that this value is not a fee rate. It is a value to modify absolute fee of the TX.\n"
            "                  The fee is not actually paid, only the algorithm for selecting transactions into a block\n"
            "                  considers the transaction as it would have paid a higher (or lower) fee."},
                },
                RPCResult{
                    RPCResult::Type::BOOL, "", "Returns true"},
                RPCExamples{
                    HelpExampleCli("prioritisetransaction", "\"txid\" 0.0 10000")
            + HelpExampleRpc("prioritisetransaction", "\"txid\", 0.0, 10000")
                },
            }.Check(request);

    LOCK(cs_main);

    uint256 hash(ParseHashV(request.params[0], "txid"));
    CAmount nAmount = request.params[2].get_int64();

    if (!(request.params[1].isNull() || request.params[1].get_real() == 0)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Priority is no longer supported, dummy argument to prioritisetransaction must be 0.");
    }

    EnsureMemPool().PrioritiseTransaction(hash, nAmount);
    return true;
}


// NOTE: Assumes a conclusive result; if result is inconclusive, it must be handled by caller
static UniValue BIP22ValidationResult(const BlockValidationState& state)
{
    if (state.IsValid())
        return NullUniValue;

    if (state.IsError())
        throw JSONRPCError(RPC_VERIFY_ERROR, state.ToString());
    if (state.IsInvalid())
    {
        std::string strRejectReason = state.GetRejectReason();
        if (strRejectReason.empty())
            return "rejected";
        return strRejectReason;
    }
    // Should be impossible
    return "valid?";
}

static UniValue getblocktemplate(const JSONRPCRequest& request)
{
            RPCHelpMan{"getblocktemplate",
                "\nIf the request parameters include a 'mode' key, that is used to explicitly select between the default 'template' request or a 'proposal'.\n"
                "It returns data needed to construct a block to work on.\n"
                "For full specification, see BIPs 22, 23, 9, and 145:\n"
                "    https://github.com/bitcoin/bips/blob/master/bip-0022.mediawiki\n"
                "    https://github.com/bitcoin/bips/blob/master/bip-0023.mediawiki\n"
                "    https://github.com/bitcoin/bips/blob/master/bip-0009.mediawiki#getblocktemplate_changes\n"
                "    https://github.com/bitcoin/bips/blob/master/bip-0145.mediawiki\n",
                {
                    {"template_request", RPCArg::Type::OBJ, "{}", "Format of the template",
                        {
                            {"mode", RPCArg::Type::STR, /* treat as named arg */ RPCArg::Optional::OMITTED_NAMED_ARG, "This must be set to \"template\", \"proposal\" (see BIP 23), or omitted"},
                            {"capabilities", RPCArg::Type::ARR, /* treat as named arg */ RPCArg::Optional::OMITTED_NAMED_ARG, "A list of strings",
                                {
                                    {"support", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "client side supported feature, 'longpoll', 'coinbasetxn', 'coinbasevalue', 'proposal', 'serverlist', 'workid'"},
                                },
                                },
                        },
                        "\"template_request\""},
                },
                RPCResult{
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "version", "The preferred block version"},
                        {RPCResult::Type::ARR, "rules", "specific block rules that are to be enforced",
                            {
                                {RPCResult::Type::STR, "", "rulename"},
                            }},
                        {RPCResult::Type::STR, "previousblockhash", "The hash of current highest block"},
                        {RPCResult::Type::ARR, "", "contents of non-coinbase transactions that should be included in the next block",
                            {
                                {RPCResult::Type::OBJ, "", "",
                                    {
                                        {RPCResult::Type::STR_HEX, "data", "transaction data encoded in hexadecimal (byte-for-byte)"},
                                        {RPCResult::Type::STR_HEX, "txid", "transaction id encoded in little-endian hexadecimal"},
                                        {RPCResult::Type::STR_HEX, "hash", "hash encoded in little-endian hexadecimal (including witness data)"},
                                        {RPCResult::Type::ARR, "depends", "array of numbers",
                                            {
                                                {RPCResult::Type::NUM, "", "transactions before this one (by 1-based index in 'transactions' list) that must be present in the final block if this one is"},
                                            }},
                                        {RPCResult::Type::NUM, "fee", "difference in value between transaction inputs and outputs (in satoshis); for coinbase transactions, this is a negative Number of the total collected block fees (ie, not including the block subsidy); if key is not present, fee is unknown and clients MUST NOT assume there isn't one"},
                                        {RPCResult::Type::NUM, "sigops", "total SigOps cost, as counted for purposes of block limits; if key is not present, sigop cost is unknown and clients MUST NOT assume it is zero"},
                                        {RPCResult::Type::NUM, "weight", "total transaction weight, as counted for purposes of block limits"},
                                    }},
                            }},
                        {RPCResult::Type::OBJ, "coinbaseaux", "data that should be included in the coinbase's scriptSig content",
                        {
                            {RPCResult::Type::ELISION, "", ""},
                        }},
                        {RPCResult::Type::NUM, "coinbasevalue", "maximum allowable input to coinbase transaction, including the generation award and transaction fees (in satoshis)"},
                        {RPCResult::Type::OBJ, "coinbasetxn", "information for coinbase transaction",
                        {
                            {RPCResult::Type::ELISION, "", ""},
                        }},
                        {RPCResult::Type::STR, "target", "The hash target"},
                        {RPCResult::Type::NUM_TIME, "mintime", "The minimum timestamp appropriate for the next block time, expressed in " + UNIX_EPOCH_TIME},
                        {RPCResult::Type::ARR, "mutable", "list of ways the block template may be changed",
                            {
                                {RPCResult::Type::STR, "value", "A way the block template may be changed, e.g. 'time', 'transactions', 'prevblock'"},
                            }},
                        {RPCResult::Type::STR_HEX, "noncerange", "A range of valid nonces"},
                        {RPCResult::Type::NUM, "sigoplimit", "limit of sigops in blocks"},
                        {RPCResult::Type::NUM, "sizelimit", "limit of block size"},
                        {RPCResult::Type::NUM, "weightlimit", "limit of block weight"},
                        {RPCResult::Type::NUM_TIME, "curtime", "current timestamp in " + UNIX_EPOCH_TIME},
                        {RPCResult::Type::STR, "bits", "compressed target of next block"},
                        {RPCResult::Type::NUM, "height", "The height of the next block"},
                    }},
                RPCExamples{
                    HelpExampleCli("getblocktemplate", "'{\"capabilities\": [\"coinbasetxn\", \"coinbasevalue\", \"longpoll\", \"workid\"]}'")
            + HelpExampleRpc("getblocktemplate", "{\"capabilities\": [\"coinbasetxn\", \"coinbasevalue\", \"longpoll\", \"workid\"]}")
                },
            }.Check(request);

    LOCK(cs_main);

    std::string strMode = "template";
    UniValue lpval = NullUniValue;
    std::set<std::string> setClientRules;
    if (!request.params[0].isNull())
    {
        const UniValue& oparam = request.params[0].get_obj();
        const UniValue& modeval = find_value(oparam, "mode");
        if (modeval.isStr())
            strMode = modeval.get_str();
        else if (modeval.isNull())
        {
            /* Do nothing */
        }
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");
        lpval = find_value(oparam, "longpollid");

        if (strMode == "proposal")
        {
            const UniValue& dataval = find_value(oparam, "data");
            if (!dataval.isStr())
                throw JSONRPCError(RPC_TYPE_ERROR, "Missing data String key for proposal");

            CBlock block;
            if (!DecodeHexBlk(block, dataval.get_str()))
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");

            uint256 hash = block.GetHash();
            const CBlockIndex* pindex = LookupBlockIndex(hash);
            if (pindex) {
                if (pindex->IsValid(BLOCK_VALID_SCRIPTS))
                    return "duplicate";
                if (pindex->nStatus & BLOCK_FAILED_MASK)
                    return "duplicate-invalid";
                return "duplicate-inconclusive";
            }

            CBlockIndex* const pindexPrev = ::ChainActive().Tip();
            // TestBlockValidity only supports blocks built on the current Tip
            if (block.hashPrevBlock != pindexPrev->GetBlockHash())
                return "inconclusive-not-best-prevblk";
            BlockValidationState state;
            TestBlockValidity(state, Params(), block, pindexPrev, false, true);
            return BIP22ValidationResult(state);
        }
    }

    if (strMode != "template")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");

    if(!g_rpc_node->connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    if (g_rpc_node->connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0)
        throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, PACKAGE_NAME " is not connected!");

    if (::ChainstateActive().IsInitialBlockDownload())
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, PACKAGE_NAME " is in initial sync and waiting for blocks...");

    static unsigned int nTransactionsUpdatedLast;
    const CTxMemPool& mempool = EnsureMemPool();

    if (!lpval.isNull())
    {
        // Wait to respond until either the best block changes, OR a minute has passed and there are more transactions
        uint256 hashWatchedChain;
        std::chrono::steady_clock::time_point checktxtime;
        unsigned int nTransactionsUpdatedLastLP;

        if (lpval.isStr())
        {
            // Format: <hashBestChain><nTransactionsUpdatedLast>
            std::string lpstr = lpval.get_str();

            hashWatchedChain = ParseHashV(lpstr.substr(0, 64), "longpollid");
            nTransactionsUpdatedLastLP = atoi64(lpstr.substr(64));
        }
        else
        {
            // NOTE: Spec does not specify behaviour for non-string longpollid, but this makes testing easier
            hashWatchedChain = ::ChainActive().Tip()->GetBlockHash();
            nTransactionsUpdatedLastLP = nTransactionsUpdatedLast;
        }

        // Release lock while waiting
        LEAVE_CRITICAL_SECTION(cs_main);
        {
            checktxtime = std::chrono::steady_clock::now() + std::chrono::minutes(1);

            WAIT_LOCK(g_best_block_mutex, lock);
            while (g_best_block == hashWatchedChain && IsRPCRunning())
            {
                if (g_best_block_cv.wait_until(lock, checktxtime) == std::cv_status::timeout)
                {
                    // Timeout: Check transactions for update
                    // without holding the mempool lock to avoid deadlocks
                    if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLastLP)
                        break;
                    checktxtime += std::chrono::seconds(10);
                }
            }
        }
        ENTER_CRITICAL_SECTION(cs_main);

        if (!IsRPCRunning())
            throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
        // TODO: Maybe recheck connections/IBD and (if something wrong) send an expires-immediately template to stop miners?
    }

    // Update block
    static CBlockIndex* pindexPrev;
    static int64_t nStart;
    static std::unique_ptr<CBlockTemplate> pblocktemplate;
    if (pindexPrev != ::ChainActive().Tip() ||
        (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 5))
    {
        // Clear pindexPrev so future calls make a new block, despite any failures from here on
        pindexPrev = nullptr;

        // Store the pindexBest used before CreateNewBlock, to avoid races
        nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
        CBlockIndex* pindexPrevNew = ::ChainActive().Tip();
        nStart = GetTime();

        // Create new block
        CScript scriptDummy = CScript() << OP_TRUE;
        pblocktemplate = BlockAssembler(mempool, Params()).CreateNewBlock(scriptDummy);
        if (!pblocktemplate)
            throw JSONRPCError(RPC_OUT_OF_MEMORY, "Out of memory");

        // Need to update only after we know CreateNewBlock succeeded
        pindexPrev = pindexPrevNew;
    }
    CHECK_NONFATAL(pindexPrev);
    CBlock* pblock = &pblocktemplate->block; // pointer for convenience

    // Update nTime
    UpdateTime(pblock);
    pblock->nNonce = 0;

    UniValue aCaps(UniValue::VARR); aCaps.push_back("proposal");

    UniValue transactions(UniValue::VARR);
    std::map<uint256, int64_t> setTxIndex;
    int i = 0;
    for (const auto& it : pblock->vtx) {
        const CTransaction& tx = *it;
        uint256 txHash = tx.GetHash();
        setTxIndex[txHash] = i++;

        if (tx.IsCoinBase())
            continue;

        UniValue entry(UniValue::VOBJ);

        entry.pushKV("data", EncodeHexTx(tx));
        entry.pushKV("txid", txHash.GetHex());
        entry.pushKV("hash", tx.GetWitnessHash().GetHex());

        UniValue deps(UniValue::VARR);
        for (const CTxIn &in : tx.vin)
        {
            if (setTxIndex.count(in.prevout.hash))
                deps.push_back(setTxIndex[in.prevout.hash]);
        }
        entry.pushKV("depends", deps);

        int index_in_template = i - 1;
        entry.pushKV("fee", pblocktemplate->vTxFees[index_in_template]);
        int64_t nTxSigOps = pblocktemplate->vTxSigOpsCost[index_in_template];
        entry.pushKV("sigops", nTxSigOps);
        entry.pushKV("weight", GetTransactionWeight(tx));

        transactions.push_back(entry);
    }

    UniValue aux(UniValue::VOBJ);

    arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);

    UniValue aMutable(UniValue::VARR);
    aMutable.push_back("time");
    aMutable.push_back("transactions");
    aMutable.push_back("prevblock");

    UniValue result(UniValue::VOBJ);
    result.pushKV("capabilities", aCaps);

    UniValue aRules(UniValue::VARR);
    aRules.push_back("csv");
    aRules.push_back("!segwit");
    result.pushKV("version", pblock->nVersion); // XXX: We Could do a little hack to keep veriumMiner working
    result.pushKV("rules", aRules);

    result.pushKV("previousblockhash", pblock->hashPrevBlock.GetHex());
    result.pushKV("transactions", transactions);
    result.pushKV("coinbaseaux", aux);
    result.pushKV("coinbasevalue", (int64_t)pblock->vtx[0]->vout[0].nValue);
    result.pushKV("longpollid", ::ChainActive().Tip()->GetBlockHash().GetHex() + ToString(nTransactionsUpdatedLast));
    result.pushKV("target", hashTarget.GetHex());
    result.pushKV("mintime", (int64_t)pindexPrev->GetMedianTimePast()+1);
    result.pushKV("mutable", aMutable);
    result.pushKV("noncerange", "00000000ffffffff");
    int64_t nSigOpLimit = MAX_BLOCK_SIGOPS_COST;
    int64_t nSizeLimit = MAX_BLOCK_SERIALIZED_SIZE;
    result.pushKV("sigoplimit", nSigOpLimit);
    result.pushKV("sizelimit", nSizeLimit);
    result.pushKV("curtime", pblock->GetBlockTime());
    result.pushKV("bits", strprintf("%08x", pblock->nBits));
    result.pushKV("height", (int64_t)(pindexPrev->nHeight+1));

    return result;
}

class submitblock_StateCatcher final : public CValidationInterface
{
public:
    uint256 hash;
    bool found;
    BlockValidationState state;

    explicit submitblock_StateCatcher(const uint256 &hashIn) : hash(hashIn), found(false), state() {}

protected:
    void BlockChecked(const CBlock& block, const BlockValidationState& stateIn) override {
        if (block.GetHash() != hash)
            return;
        found = true;
        state = stateIn;
    }
};

static UniValue submitblock(const JSONRPCRequest& request)
{
    // We allow 2 arguments for compliance with BIP22. Argument 2 is ignored.
            RPCHelpMan{"submitblock",
                "\nAttempts to submit new block to network.\n"
                "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.\n",
                {
                    {"hexdata", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hex-encoded block data to submit"},
                    {"dummy", RPCArg::Type::STR, /* default */ "ignored", "dummy value, for compatibility with BIP22. This value is ignored."},
                },
                RPCResult{RPCResult::Type::NONE, "", "Returns JSON Null when valid, a string according to BIP22 otherwise"},
                RPCExamples{
                    HelpExampleCli("submitblock", "\"mydata\"")
            + HelpExampleRpc("submitblock", "\"mydata\"")
                },
            }.Check(request);

    std::shared_ptr<CBlock> blockptr = std::make_shared<CBlock>();
    CBlock& block = *blockptr;
    if (!DecodeHexBlk(block, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");
    }

    if (block.vtx.empty() || !block.vtx[0]->IsCoinBase()) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block does not start with a coinbase");
    }

    uint256 hash = block.GetHash();
    {
        LOCK(cs_main);
        const CBlockIndex* pindex = LookupBlockIndex(hash);
        if (pindex) {
            if (pindex->IsValid(BLOCK_VALID_SCRIPTS)) {
                return "duplicate";
            }
            if (pindex->nStatus & BLOCK_FAILED_MASK) {
                return "duplicate-invalid";
            }
        }
    }

    {
        LOCK(cs_main);
        const CBlockIndex* pindex = LookupBlockIndex(block.hashPrevBlock);
        if (pindex) {
            UpdateUncommittedBlockStructures(block, pindex, Params().GetConsensus());
        }
    }

    bool new_block;
    auto sc = std::make_shared<submitblock_StateCatcher>(block.GetHash());
    RegisterSharedValidationInterface(sc);
    bool accepted = ProcessNewBlock(Params(), blockptr, /* fForceProcessing */ true, /* fNewBlock */ &new_block);
    UnregisterSharedValidationInterface(sc);
    if (!new_block && accepted) {
        return "duplicate";
    }
    if (!sc->found) {
        return "inconclusive";
    }
    return BIP22ValidationResult(sc->state);
}

static UniValue submitheader(const JSONRPCRequest& request)
{
            RPCHelpMan{"submitheader",
                "\nDecode the given hexdata as a header and submit it as a candidate chain tip if valid."
                "\nThrows when the header is invalid.\n",
                {
                    {"hexdata", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the hex-encoded block header data"},
                },
                RPCResult{
                    RPCResult::Type::NONE, "", "None"},
                RPCExamples{
                    HelpExampleCli("submitheader", "\"aabbcc\"") +
                    HelpExampleRpc("submitheader", "\"aabbcc\"")
                },
            }.Check(request);

    CBlockHeader h;
    if (!DecodeHexBlockHeader(h, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block header decode failed");
    }
    {
        LOCK(cs_main);
        if (!LookupBlockIndex(h.hashPrevBlock)) {
            throw JSONRPCError(RPC_VERIFY_ERROR, "Must submit previous header (" + h.hashPrevBlock.GetHex() + ") first");
        }
    }

    BlockValidationState state;
    ProcessNewBlockHeaders({h}, state, Params(), nullptr);
    if (state.IsValid()) return NullUniValue;
    if (state.IsError()) {
        throw JSONRPCError(RPC_VERIFY_ERROR, state.ToString());
    }
    throw JSONRPCError(RPC_VERIFY_ERROR, state.GetRejectReason());
}


UniValue minerstatus(const JSONRPCRequest& request)
{
    RPCHelpMan{"minerstatus",
        "\nMining status (Verium only)",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "status", "Mining status (active/stopped)"},
            }
        },
        RPCExamples{
            HelpExampleCli("minerstatus", "")
    + HelpExampleRpc("minerstatus", "")
        },
    }.Check(request);

    if( Params().IsVericoin())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Action impossible on Vericoin");

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("status",   ( IsMining() ? "active" : "stopped"));

    return obj;
}

UniValue minerstart(const JSONRPCRequest& request)
{
    RPCHelpMan{"minerstart",
        "\nStart mining (Verium only)",
        {
            {"nthreads", RPCArg::Type::NUM, RPCArg::Optional::NO, "Number of thread to allocate to mining."},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "status", "Mining status (active/stopped)"},
                {RPCResult::Type::NUM, "nthreads", "Number of thread allocated"},
            }
        },
        RPCExamples{
            HelpExampleCli("minerstart", "")
    + HelpExampleRpc("minerstart", "")
        },
    }.Check(request);

    if( Params().IsVericoin())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Action impossible on Vericoin");

    if(!g_rpc_node->connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(wallet.get(), request.fHelp)) {
        return false;
    }

    int nThreads = request.params[0].get_int();

    LOCK(cs_main);

    GenerateVerium(true, wallet, nThreads, g_rpc_node->connman.get(), g_rpc_node->mempool);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("status",   "active");
    obj.pushKV("nthreads", nThreads);

    return obj;
}

UniValue minerstop(const JSONRPCRequest& request)
{
    RPCHelpMan{"minerstop",
        "\nStop mining (Verium only)",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "status", "Mining status (active/stopped)"},
                {RPCResult::Type::NUM, "nthreads", "Number of thread allocated"},
            }
        },
        RPCExamples{
            HelpExampleCli("minerstop", "")
    + HelpExampleRpc("minerstop", "")
        },
    }.Check(request);

    if( Params().IsVericoin())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Action impossible on Vericoin");

    if(!g_rpc_node->connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");


    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(wallet.get(), request.fHelp)) {
        return false;
    }

    LOCK(cs_main);

    GenerateVerium(false, wallet, 0, g_rpc_node->connman.get(), g_rpc_node->mempool);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("status",   "stopped");
    obj.pushKV("nthreads", 0);

    return obj;
}

UniValue stakingstatus(const JSONRPCRequest& request)
{
    RPCHelpMan{"stakingstatus",
        "\nstaking status (Vericoin only)",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "status", "Staking status (active/stopped)"},
            }
        },
        RPCExamples{
            HelpExampleCli("stakingstatus", "")
    + HelpExampleRpc("stakingstatus", "")
        },
    }.Check(request);

    if( ! Params().IsVericoin())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Action impossible on Verium");

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("status",   ( IsStaking() ? "active" : "stopped"));

    return obj;
}

UniValue stakingstart(const JSONRPCRequest& request)
{
    RPCHelpMan{"stakingstart",
        "\nStart staking (Vericoin only)",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "status", "Staking status (active/stopped)"},
            }
        },
        RPCExamples{
            HelpExampleCli("stakingstart", "")
    + HelpExampleRpc("stakingstart", "")
        },
    }.Check(request);

    if( ! Params().IsVericoin())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Action impossible on Verium");

    if(!g_rpc_node->connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(wallet.get(), request.fHelp)) {
        return false;
    }

    LOCK(cs_main);

    GenerateVericoin(true, wallet, g_rpc_node->connman.get(), g_rpc_node->mempool);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("status",   "active");

    return obj;
}

UniValue stakingstop(const JSONRPCRequest& request)
{
    RPCHelpMan{"stakingstop",
        "\nStop staking (Vericoin only)",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "status", "Staking status (active/stopped)"},
            }
        },
        RPCExamples{
            HelpExampleCli("stakingstop", "")
    + HelpExampleRpc("stakingstop", "")
        },
    }.Check(request);

    if( ! Params().IsVericoin())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Action impossible on Verium");

    if(!g_rpc_node->connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");


    std::shared_ptr<CWallet> const wallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(wallet.get(), request.fHelp)) {
        return false;
    }

    LOCK(cs_main);

    GenerateVericoin(false, wallet,  g_rpc_node->connman.get(), g_rpc_node->mempool);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("status",   "stopped");

    return obj;
}

void RegisterMiningRPCCommands(CRPCTable &t)
{
// clang-format off
static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         argNames
  //  --------------------- ------------------------  -----------------------  ----------
    { "mining",             "getmininginfo",          &getmininginfo,          {} },
    { "mining",             "prioritisetransaction",  &prioritisetransaction,  {"txid","dummy","fee_delta"} },
    { "mining",             "getblocktemplate",       &getblocktemplate,       {"template_request"} },
    { "mining",             "submitblock",            &submitblock,            {"hexdata","dummy"} },
    { "mining",             "submitheader",           &submitheader,           {"hexdata"} },

    { "miner",              "minerstatus",            &minerstatus,            {} },
    { "miner",              "minerstop",              &minerstop,              {} },
    { "miner",              "minerstart",             &minerstart,             {"nthreads"} },

    { "staking",            "stakingstatus",          &stakingstatus,            {} },
    { "staking",            "stakingstop",            &stakingstop,              {} },
    { "staking",            "stakingstart",           &stakingstart,             {} },

    { "generating",         "generatetoaddress",      &generatetoaddress,      {"nblocks","address","maxtries"} },
    { "generating",         "generatetodescriptor",   &generatetodescriptor,   {"num_blocks","descriptor","maxtries"} },
};
// clang-format on

    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
