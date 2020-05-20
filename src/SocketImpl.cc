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

#include "SocketImpl.h"

#include <PerfUtils/Cycles.h>

#include "Debug.h"
#include "Perf.h"
#include "RooPCImpl.h"
#include "ServerTaskImpl.h"

namespace Roo {

/**
 * Construct a SocketImpl.
 *
 * @param transport
 *      Homa transport to which this socket has exclusive access.
 */
SocketImpl::SocketImpl(Homa::Transport* transport)
    : transport(transport)
    , socketId(transport->getId())
    , nextSequenceNumber(1)
    , mutex()
    , rpcs()
    , pendingTasks()
    , detachedTasks()
{}

/**
 * SocketImpl destructor.
 */
SocketImpl::~SocketImpl() {}

/**
 * @copydoc Roo::Socket::allocRooPC()
 */
Roo::unique_ptr<RooPC>
SocketImpl::allocRooPC()
{
    SpinLock::Lock lock_socket(mutex);
    Proto::RooId rooId = allocTaskId();
    RooPCImpl* rpc = new RooPCImpl(this, rooId);
    rpcs.insert({rooId, rpc});
    return Roo::unique_ptr<RooPC>(rpc);
}

/**
 * @copydoc Roo::Socket::receive()
 */
Roo::unique_ptr<ServerTask>
SocketImpl::receive()
{
    SpinLock::Lock lock_socket(mutex);
    Roo::unique_ptr<ServerTask> task;
    if (!pendingTasks.empty()) {
        task = Roo::unique_ptr<ServerTask>(pendingTasks.front());
        pendingTasks.pop_front();
    }
    return task;
}

/**
 * @copydoc Roo::Socket::poll()
 */
void
SocketImpl::poll()
{
    transport->poll();
    bool idle = true;
    uint64_t start_tsc = PerfUtils::Cycles::rdtsc();
    // Process incoming messages
    for (Homa::unique_ptr<Homa::InMessage> message = transport->receive();
         message; message = std::move(transport->receive())) {
        idle = false;
        Proto::HeaderCommon common;
        message->get(0, &common, sizeof(common));
        if (common.opcode == Proto::Opcode::Request) {
            // Incoming message is a request.
            Proto::RequestHeader header;
            message->get(0, &header, sizeof(header));
            Perf::counters.rx_message_bytes.add(message->length() -
                                                sizeof(header));
            ServerTaskImpl* task = new ServerTaskImpl(
                this, allocTaskId(), &header, std::move(message));
            SpinLock::Lock lock_socket(mutex);
            pendingTasks.push_back(task);
        } else if (common.opcode == Proto::Opcode::Response) {
            // Incoming message is a response
            Proto::ResponseHeader header;
            message->get(0, &header, sizeof(header));
            Perf::counters.rx_message_bytes.add(message->length() -
                                                sizeof(header));
            SpinLock::Lock lock_socket(mutex);
            auto it = rpcs.find(header.rooId);
            if (it != rpcs.end()) {
                RooPCImpl* rpc = it->second;
                rpc->handleResponse(&header, std::move(message));
            } else {
                // There is no RooPC waiting for this message.
            }
        } else if (common.opcode == Proto::Opcode::Manifest) {
            Proto::Manifest manifest;
            message->get(0, &manifest, sizeof(manifest));
            SpinLock::Lock lock_socket(mutex);
            auto it = rpcs.find(manifest.rooId);
            if (it != rpcs.end()) {
                RooPCImpl* rpc = it->second;
                rpc->handleManifest(&manifest, std::move(message));
            } else {
                // There is no RooPC waiting for this manifest.
            }
        } else {
            WARNING("Unexpected protocol message received.");
        }
    }

    // Check detached ServerTasks
    {
        SpinLock::Lock lock_socket(mutex);
        auto it = detachedTasks.begin();
        while (it != detachedTasks.end()) {
            ServerTaskImpl* task = *it;
            bool not_done = task->poll();
            if (not_done) {
                ++it;
            } else {
                // ServerTask is done polling
                it = detachedTasks.erase(it);
                delete task;
                idle = false;
            }
        }
    }

    // Track cycles spent processing
    uint64_t elapsed_cycles = PerfUtils::Cycles::rdtsc() - start_tsc;
    if (!idle) {
        Perf::counters.active_cycles.add(elapsed_cycles);
    } else {
        Perf::counters.idle_cycles.add(elapsed_cycles);
    }
}

/**
 * Discard a previously allocated RooPC.
 */
void
SocketImpl::dropRooPC(RooPCImpl* rpc)
{
    SpinLock::Lock lock_socket(mutex);
    rpcs.erase(rpc->getId());
    delete rpc;
}

/**
 * Pass custody of a detached ServerTask to this socket so that this socket
 * can ensure its outbound message are completely sent.
 */
void
SocketImpl::remandTask(ServerTaskImpl* task)
{
    SpinLock::Lock lock_socket(mutex);
    detachedTasks.push_back(task);
}

/**
 * Return a new unique TaskId.
 */
Proto::TaskId
SocketImpl::allocTaskId()
{
    return Proto::TaskId(
        socketId, nextSequenceNumber.fetch_add(1, std::memory_order_relaxed));
}

}  // namespace Roo
