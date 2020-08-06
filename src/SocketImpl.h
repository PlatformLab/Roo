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

#ifndef ROO_SOCKETIMPL_H
#define ROO_SOCKETIMPL_H

#include <Homa/Homa.h>
#include <Roo/Roo.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <unordered_map>

#include "Proto.h"
#include "SpinLock.h"

namespace Roo {

// Forward declaration
class RooPCImpl;
class ServerTaskImpl;

/**
 * Implementation of Roo::Socket.
 */
class SocketImpl : public Socket {
  public:
    explicit SocketImpl(Homa::Transport* transport);
    virtual ~SocketImpl();
    virtual Roo::unique_ptr<RooPC> allocRooPC();
    virtual Roo::unique_ptr<ServerTask> receive();
    virtual void poll();
    virtual Homa::Driver* getDriver()
    {
        return transport->getDriver();
    }

    void dropRooPC(RooPCImpl* rpc);
    void remandTask(ServerTaskImpl* task);

    /// Transport through which messages can be sent and received.
    Homa::Transport* const transport;

  private:
    /**
     * Associates a timeout value with an object.
     */
    template <typename T>
    struct Timeout {
        /// Time when the associated object should time out.
        std::chrono::steady_clock::time_point expirationTime;
        /// The object associated with this timeout.
        T object;
    };

    void processIncomingMessages();
    void checkDetachedTasks();
    void checkClientTimeouts();
    void checkTaskTimeouts();
    Proto::TaskId allocTaskId();

    /// Identifer for this socket.  This identifer must be unique among all
    /// sockets that might communicate.
    uint64_t const socketId;

    /// Used to generate socket unique identifiers.
    std::atomic<uint64_t> nextSequenceNumber;

    // Monitor style mutex.
    SpinLock mutex;

    /// Tracks the set of RooPC objects that were initiated by this socket.
    std::unordered_map<Proto::RooId, RooPCImpl*, Proto::RooId::Hasher> rpcs;

    /// RooPC ids in increasing timeout order.
    std::list<Timeout<Proto::RooId>> rpcTimeouts;

    /// Tracks the set of live ServerTask objects managed by this socket.
    std::unordered_map<Proto::RequestId, ServerTaskImpl*,
                       Proto::RequestId::Hasher>
        tasks;

    /// Collection of ServerTask objects (incoming requests) that haven't been
    /// requested by the application.
    std::deque<ServerTaskImpl*> pendingTasks;

    /// ServerTask objects that have been processed by the application and
    /// remanded to the care of the Socket to complete transmission.
    std::deque<ServerTaskImpl*> detachedTasks;

    /// ServerTask objects have completed transmission and are waiting to be
    /// garbage collected after a timeout. ServerTask objects are held in
    /// timeout order.
    std::list<Timeout<ServerTaskImpl*>> taskTimeouts;
};

}  // namespace Roo

#endif  // ROO_SOCKETIMPL_H
