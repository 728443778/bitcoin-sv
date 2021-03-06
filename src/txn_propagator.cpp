// Copyright (c) 2018 The Bitcoin SV developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txn_propagator.h"
#include "net.h"

// When we get C++17 we should loose this redundant definition, until then it's required.
constexpr unsigned CTxnPropagator::DEFAULT_RUN_FREQUENCY_MILLIS;

/** Constructor */
CTxnPropagator::CTxnPropagator()
{
    // Configure our running frequency
    auto runFreq { gArgs.GetArg("-txnpropagationfreq", DEFAULT_RUN_FREQUENCY_MILLIS) };
    mRunFrequency = std::chrono::milliseconds {runFreq};

    // Launch our threads
    mNewTxnsThread = std::thread(&CTxnPropagator::threadNewTxnHandler, this);
}

/** Destructor */
CTxnPropagator::~CTxnPropagator()
{
    shutdown();
}

/** Get the frequency we run */
std::chrono::milliseconds CTxnPropagator::getRunFrequency() const
{
    std::unique_lock<std::mutex> lock { mNewTxnsMtx };
    return mRunFrequency;
}

/** Set the frequency we run */
void CTxnPropagator::setRunFrequency(const std::chrono::milliseconds& freq)
{
    std::unique_lock<std::mutex> lock { mNewTxnsMtx };
    mRunFrequency = freq;

    // Also wake up the processing thread so that it is then rescheduled at the right frequency
    mNewTxnsCV.notify_one();
}

/** Get the number of queued new transactions awaiting processing */
size_t CTxnPropagator::getNewTxnQueueLength() const
{
    std::unique_lock<std::mutex> lock { mNewTxnsMtx };
    return mNewTxns.size();
}

/** Handle a new transaction */
void CTxnPropagator::newTransaction(const CTxnSendingDetails& txn)
{
    // Add it to the list of new transactions
    std::unique_lock<std::mutex> lock { mNewTxnsMtx };
    mNewTxns.push_back(txn);
}

/** Remove some old transactions */
void CTxnPropagator::removeTransactions(const std::vector<CTransactionRef>& txns)
{
    LogPrint(BCLog::TXNPROP, "Purging %d transactions\n", txns.size());

    // Create sorted list of CTxnSendingDetails as required to remove them from
    // a nodes list.
    std::vector<CTxnSendingDetails> txnDetails {};
    txnDetails.reserve(txns.size());
    CompareTxnSendingDetails comp { &mempool };
    for(const CTransactionRef& txn : txns)
    {   
        CInv inv { MSG_TX, txn->GetId() };
        txnDetails.emplace_back(inv, txn);
    }
    {
        LOCK(mempool.cs);
        std::sort(txnDetails.begin(), txnDetails.end(), comp);
    }

    // Filter list of new transactions
    {
        std::vector<CTxnSendingDetails> filteredNewTxns {};

        // Ensure we always take our lock first, then the mempool lock
        std::unique_lock<std::mutex> lock { mNewTxnsMtx };
        LOCK(mempool.cs);

        std::sort(mNewTxns.begin(), mNewTxns.end(), comp);
        std::set_difference(mNewTxns.begin(), mNewTxns.end(), txnDetails.begin(), txnDetails.end(),
            std::inserter(filteredNewTxns, filteredNewTxns.begin()), comp);
        mNewTxns = std::move(filteredNewTxns);
    }

    // Update lists of pending transactions for each node
    {
        LOCK(mempool.cs);
        auto results { g_connman->ParallelForEachNode([&txnDetails](const CNodePtr& node) { node->RemoveTxnsFromInventory(txnDetails); }) };

        // Wait for all nodes to finish processing so we can safely release the mempool lock
        for(auto& result : results)
            result.wait();
    }
}

/** Shutdown and clean up */
void CTxnPropagator::shutdown()
{
    // Only shutdown once
    bool expected {true};
    if(mRunning.compare_exchange_strong(expected, false))
    {
        // Shutdown threads
        {
            std::unique_lock<std::mutex> lock { mNewTxnsMtx };
            mNewTxnsCV.notify_one();
        }

        mNewTxnsThread.join();
    }
}

/** Thread entry point for new transaction queue handling */
void CTxnPropagator::threadNewTxnHandler() noexcept
{
    try
    {
        LogPrint(BCLog::TXNPROP, "New transaction handling thread starting\n");

        while(mRunning)
        {
            // Run every few seconds or until stopping
            std::unique_lock<std::mutex> lock { mNewTxnsMtx };
            mNewTxnsCV.wait_for(lock, mRunFrequency);
            if(mRunning && !mNewTxns.empty())
            {
                // Process all new transactions
                LogPrint(BCLog::TXNPROP, "Got %d new transactions\n", mNewTxns.size());
                processNewTransactions();
            }
        }

        LogPrint(BCLog::TXNPROP, "New transaction handling thread stopping\n");
    }
    catch(...)
    {
        LogPrint(BCLog::TXNPROP, "Unexpected exception in new transaction thread\n");
    }
}

/**
* Process all new transactions.
* Already holds mNewTxnsMtx.
*/
void CTxnPropagator::processNewTransactions()
{
    {
        // Take the mempool lock so we can do all the difficult txn sorting and node updating in parallel.
        LOCK(mempool.cs);
        auto results { g_connman->ParallelForEachNode([this](const CNodePtr& node) { node->AddTxnsToInventory(mNewTxns); }) };

        // Wait for all nodes to finish processing so we can safely release the mempool lock
        for(auto& result : results)
            result.wait();
    }

    // Clear new transactions list
    mNewTxns.clear();
}

