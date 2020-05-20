/* Copyright (c) 2020, Stanford University
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "Perf.h"

#include <Homa/Perf.h>
#include <PerfUtils/Cycles.h>

#include <mutex>
#include <unordered_set>

namespace Roo {
namespace Perf {

namespace Internal {

/**
 * Protects access to globalCounters and perThreadCounters
 */
std::mutex mutex;

/**
 * Contains statistics information for any thread that as already exited.
 */
Counters globalCounters;

/**
 * Set of all counters for all threads.
 */
std::unordered_set<const Counters*> perThreadCounters;

}  // namespace Internal

// Init thread local thread counters
thread_local ThreadCounters counters;

/**
 * Construct and register a new per thread set of counters.
 */
ThreadCounters::ThreadCounters()
{
    std::lock_guard<std::mutex> lock(Internal::mutex);
    Internal::perThreadCounters.insert(this);
}

/**
 * Deregister and destruct a per thread set of counters.
 */
ThreadCounters::~ThreadCounters()
{
    std::lock_guard<std::mutex> lock(Internal::mutex);
    Internal::globalCounters.add(this);
    Internal::perThreadCounters.erase(this);
}

/**
 */
void
getStats(Stats* stats)
{
    std::lock_guard<std::mutex> lock(Internal::mutex);
    stats->timestamp = PerfUtils::Cycles::rdtsc();
    stats->cycles_per_second = PerfUtils::Cycles::perSecond();

    Counters output;
    output.add(&Internal::globalCounters);
    for (const Counters* counters : Internal::perThreadCounters) {
        output.add(counters);
    }
    output.dumpStats(stats);

    // Include Homa stats
    Homa::Perf::Stats homa_stats;
    Homa::Perf::getStats(&homa_stats);

    stats->active_cycles += homa_stats.active_cycles;
    stats->idle_cycles += homa_stats.idle_cycles;
    stats->transport_tx_bytes = homa_stats.tx_bytes;
    stats->transport_rx_bytes = homa_stats.rx_bytes;
    stats->tx_data_pkts = homa_stats.tx_data_pkts;
    stats->rx_data_pkts = homa_stats.rx_data_pkts;
    stats->tx_grant_pkts = homa_stats.tx_grant_pkts;
    stats->rx_grant_pkts = homa_stats.rx_grant_pkts;
    stats->tx_done_pkts = homa_stats.tx_done_pkts;
    stats->rx_done_pkts = homa_stats.rx_done_pkts;
    stats->tx_resend_pkts = homa_stats.tx_resend_pkts;
    stats->rx_resend_pkts = homa_stats.rx_resend_pkts;
    stats->tx_busy_pkts = homa_stats.tx_busy_pkts;
    stats->rx_busy_pkts = homa_stats.rx_busy_pkts;
    stats->tx_ping_pkts = homa_stats.tx_ping_pkts;
    stats->rx_ping_pkts = homa_stats.rx_ping_pkts;
    stats->tx_unknown_pkts = homa_stats.tx_unknown_pkts;
    stats->rx_unknown_pkts = homa_stats.rx_unknown_pkts;
    stats->tx_error_pkts = homa_stats.tx_error_pkts;
    stats->rx_error_pkts = homa_stats.rx_error_pkts;
}

}  // namespace Perf
}  // namespace Roo
