/* Copyright (c) 2019-2020, Stanford University
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

#ifndef ROO_TIMEOUT_H
#define ROO_TIMEOUT_H

#include <PerfUtils/Cycles.h>

#include <atomic>

#include "Intrusive.h"

namespace Roo {

// Forward declaration.
template <typename T>
class TimeoutManager;

/**
 * A Timeout holds an object that can be added to a TimeoutManager which makes
 * it available again at a specified point in time.
 *
 * This structure is not thread-safe.
 */
template <typename T>
class Timeout {
  public:
    /**
     * Initialized a Timeout with an associating object. The object will be
     * constructed in-place using the provided arguments.
     *
     * @param args
     *      Arguments for the associated object's constructor.
     */
    template <typename... Args>
    explicit Timeout(Args&&... args)
        : object(static_cast<Args&&>(args)...)
        , expirationCycleTime(0)
        , node(this)
    {}

    /**
     * Return true if this Timeout has elapsed; false otherwise.
     *
     * @param now
     *      Optionally provided "current" timestamp cycle time. Used to avoid
     *      unnecessary calls to PerfUtils::Cycles::rdtsc() if the current time
     *      is already available to the caller.
     */
    inline bool hasElapsed(uint64_t now = PerfUtils::Cycles::rdtsc())
    {
        return now >= expirationCycleTime;
    }

    /// The object that is associated with this timeout.
    T object;

  private:
    /// Cycle timestamp when timeout should elapse.
    uint64_t expirationCycleTime;

    /// Intrusive member to help track this timeout.
    typename Intrusive::List<Timeout<T>>::Node node;

    friend class TimeoutManager<T>;
};

/**
 * Structure to keep track of multiple instances of the same kind of timeout.
 *
 * This structure is not thread-safe.
 */
template <typename T>
class TimeoutManager {
  public:
    /**
     * Construct a new TimeoutManager with a particular timeout interval.  All
     * timeouts tracked by this manager will have the same timeout interval.
     *
     */
    explicit TimeoutManager(uint64_t timeoutIntervalCycles)
        : timeoutIntervalCycles(timeoutIntervalCycles)
        , nextTimeout(UINT64_MAX)
        , list()
    {}

    /**
     * Schedule the Timeout to elapse one timeout interval from this point.  If
     * the Timeout was previously scheduled, this call will reschedule it.
     *
     * @param timeout
     *      The Timeout that should be scheduled.
     */
    inline void setTimeout(Timeout<T>* timeout)
    {
        list.remove(&timeout->node);
        timeout->expirationCycleTime =
            PerfUtils::Cycles::rdtsc() + timeoutIntervalCycles;
        list.push_back(&timeout->node);
        nextTimeout.store(list.front().expirationCycleTime,
                          std::memory_order_relaxed);
    }

    /**
     * Cancel the Timeout if it was previously scheduled.
     *
     * @param timeout
     *      The Timeout that should be canceled.
     */
    inline void cancelTimeout(Timeout<T>* timeout)
    {
        list.remove(&timeout->node);
        if (list.empty()) {
            nextTimeout.store(UINT64_MAX, std::memory_order_relaxed);
        } else {
            nextTimeout.store(list.front().expirationCycleTime,
                              std::memory_order_relaxed);
        }
    }

    /**
     * Check if any managed Timeouts have elapsed.
     *
     * This method is thread-safe but may race with the other non-thread-safe
     * methods of the TimeoutManager (e.g. concurrent calls to setTimeout() or
     * cancelTimeout() may not be reflected in the result of this method call).
     *
     * @param now
     *      Optionally provided "current" timestamp cycle time. Used to avoid
     *      unnecessary calls to PerfUtils::Cycles::rdtsc() if the current time
     *      is already available to the caller.
     */
    inline bool anyElapsed(uint64_t now = PerfUtils::Cycles::rdtsc())
    {
        return now >= nextTimeout.load(std::memory_order_relaxed);
    }

    /**
     * Check if the TimeoutManager manages no Timeouts.
     *
     * @return
     *      True, if there are no Timeouts being managed; false, otherwise.
     */
    inline bool empty() const
    {
        return list.empty();
    }

    /**
     * Return a pointer the managed timeout element that expires first.
     *
     * Calling front() an empty TimeoutManager is undefined.
     */
    inline Timeout<T>* front()
    {
        return &list.front();
    }

    /**
     * Return a const pointer the managed timeout element that expires first.
     *
     * Calling front() an empty TimeoutManager is undefined.
     */
    inline const Timeout<T>* front() const
    {
        return &list.front();
    }

  private:
    /// The number of cycles this newly scheduled timeouts would wait before
    /// they elapse.
    uint64_t timeoutIntervalCycles;

    /// The smallest timeout expiration time of all timeouts under
    /// management. Accessing this value is thread-safe.
    std::atomic<uint64_t> nextTimeout;

    /// Used to keep track of all timeouts under management.
    Intrusive::List<Timeout<T>> list;
};

}  // namespace Roo

#endif  // ROO_TIMEOUT_H
