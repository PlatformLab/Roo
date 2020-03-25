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

    Proto::RequestId allocRequestId();
    void dropRooPC(RooPCImpl* rpc);
    void remandTask(ServerTaskImpl* task);

    /// Transport through which messages can be sent and received.
    Homa::Transport* const transport;

  private:
    /// Identifer for this socket.  This identifer must be unique among all
    /// sockets that might communicate.
    uint64_t const socketId;

    /// Used to generate socket unique identifiers.
    std::atomic<uint64_t> nextSequenceNumber;

    // Monitor style mutex.
    SpinLock mutex;

    /// Tracks the set of RooPC objects that were initiated by this socket.
    std::unordered_map<Proto::RooId, RooPCImpl*, Proto::RooId::Hasher> rpcs;

    /// Collection of ServerTask objects (incoming requests) that haven't been
    /// requested by the application.
    std::deque<ServerTaskImpl*> pendingTasks;

    /// ServerTask objects that have been processed by the application and
    /// remanded to the care of the Socket to complete transmission.
    std::deque<ServerTaskImpl*> detachedTasks;
};

}  // namespace Roo

#endif  // ROO_SOCKETIMPL_H
