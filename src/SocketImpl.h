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

#include "ObjectPool.h"
#include "Proto.h"
#include "RooPCImpl.h"
#include "ServerTaskImpl.h"
#include "SpinLock.h"
#include "Timeout.h"

namespace Roo {

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
     * Collection of all socket state for a single RooPC.
     */
    struct RpcHandle {
        /// Constructor
        template <typename... Args>
        RpcHandle(Args&&... args)
            : rpc(static_cast<Args&&>(args)...)
            , timeout(this)
        {}

        /// Destructor
        ~RpcHandle() = default;

        /// RooPC object
        RooPCImpl rpc;

        /// Timeout entry associated with this RooPC
        Timeout<RpcHandle*> timeout;
    };

    /**
     * Collection of all socket state for a single ServerTask.
     */
    struct ServerTaskHandle {
        /// Constructor
        template <typename... Args>
        ServerTaskHandle(Args&&... args)
            : task(static_cast<Args&&>(args)...)
            , timeout(this)
        {}

        /// Destructor
        ~ServerTaskHandle() = default;

        /// ServerTask object
        ServerTaskImpl task;

        /// Timeout entry associated with this ServerTask
        Timeout<ServerTaskHandle*> timeout;
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

    /// Monitor style mutex.
    SpinLock mutex;

    /// RooPC allocator
    ObjectPool<RpcHandle> rpcPool;

    /// ServerTask allocator
    ObjectPool<ServerTaskHandle> taskPool;

    /// Tracks the set of RooPC objects that were initiated by this socket.
    std::unordered_map<Proto::RooId, RpcHandle*, Proto::RooId::Hasher> rpcs;

    /// Tracks the set of live ServerTask objects managed by this socket.
    std::unordered_map<Proto::RequestId, ServerTaskHandle*,
                       Proto::RequestId::Hasher>
        tasks;

    /// RooPC ids in increasing timeout order.
    TimeoutManager<RpcHandle*> rpcTimeouts;

    /// ServerTask objects have completed transmission and are waiting to be
    /// garbage collected after a timeout. ServerTask objects are held in
    /// timeout order.
    TimeoutManager<ServerTaskHandle*> taskTimeouts;

    /// Collection of ServerTask objects (incoming requests) that haven't been
    /// requested by the application.
    std::deque<ServerTaskImpl*> pendingTasks;

    /// ServerTask objects that have been processed by the application and
    /// remanded to the care of the Socket to complete transmission.
    std::deque<ServerTaskImpl*> detachedTasks;
};

}  // namespace Roo

#endif  // ROO_SOCKETIMPL_H
