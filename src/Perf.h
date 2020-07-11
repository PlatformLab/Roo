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

#ifndef ROO_PERF_H
#define ROO_PERF_H

#include <PerfUtils/Cycles.h>
#include <Roo/Perf.h>

#include <atomic>

namespace Roo {
namespace Perf {

/**
 * Collection of collected performance counters.
 */
struct Counters {
    /**
     * Wrapper class for individual counter entires to
     */
    template <typename T>
    struct Stat : private std::atomic<T> {
        /**
         * Passthrough constructor.
         */
        template <typename... Args>
        Stat(Args&&... args)
            : std::atomic<T>(static_cast<Args&&>(args)...)
        {}

        /**
         * Add the value of another Stat to this Stat.
         */
        void add(const Stat<T>& other)
        {
            this->fetch_add(other.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
        }

        /**
         * Add the given value to this Stat.
         */
        void add(T val)
        {
            this->fetch_add(val, std::memory_order_relaxed);
        }

        /**
         * Return the stat value.
         */
        T get() const
        {
            return this->load(std::memory_order_relaxed);
        }
    };

    /**
     * Default constructor.
     */
    Counters()
        : active_cycles(0)
        , idle_cycles(0)
        , tx_message_bytes(0)
        , rx_message_bytes(0)
    {}

    /**
     * Default destructor.
     */
    ~Counters() = default;

    /**
     * Add the values in other to the corresponding counters in this object.
     */
    void add(const Counters* other)
    {
        active_cycles.add(other->active_cycles);
        idle_cycles.add(other->idle_cycles);
        tx_message_bytes.add(other->tx_message_bytes);
        rx_message_bytes.add(other->rx_message_bytes);
    }

    /**
     * Export this object's counter values to a Stats structure.
     */
    void dumpStats(Stats* stats)
    {
        stats->active_cycles = active_cycles.get();
        stats->idle_cycles = idle_cycles.get();
        stats->tx_message_bytes = tx_message_bytes.get();
        stats->rx_message_bytes = rx_message_bytes.get();
    }

    /// CPU time actively processing RooPCs and ServerTask messages in cycles.
    Stat<uint64_t> active_cycles;

    /// CPU time running Roo with no work to do in cycles.
    Stat<uint64_t> idle_cycles;

    /// Number of application message bytes sent.
    Stat<uint64_t> tx_message_bytes;

    /// Number of application message bytes received.
    Stat<uint64_t> rx_message_bytes;
};

/**
 * Thread-local collection of performance counters.
 */
struct ThreadCounters : public Counters {
    ThreadCounters();
    ~ThreadCounters();
};

/**
 * Per thread counters.
 */
extern thread_local ThreadCounters counters;

/**
 * Provides a convenient way to measure multiple consecutive cycle time
 * intervals.
 */
class Timer {
  public:
    /**
     * Construct a new uninitialized Timer.
     */
    Timer()
        : split_tsc(0)
    {}

    /**
     * Return the number of cycles since the last time split was called.
     */
    inline uint64_t split()
    {
        uint64_t prev_tsc = split_tsc;
        split_tsc = PerfUtils::Cycles::rdtsc();
        return split_tsc - prev_tsc;
    }

  private:
    /// Cycle time that split was last called.
    uint64_t split_tsc;
};

}  // namespace Perf
}  // namespace Roo

#endif  // ROO_PERF_H
