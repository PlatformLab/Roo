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

#include "SessionImpl.h"

#include "Debug.h"
#include "RooPCImpl.h"
#include "ServerTaskImpl.h"

namespace Roo {

/**
 * Construct a SessionImpl.
 *
 * @param transport
 *      Homa transport to which this session has exclusive access.
 */
SessionImpl::SessionImpl(Homa::Transport* transport)
    : transport(transport)
    , sessionId(transport->getId())
    , nextSequenceNumber(1)
    , mutex()
    , rpcs()
    , pendingTasks()
    , detachedTasks()
{}

/**
 * SessionImpl destructor.
 */
SessionImpl::~SessionImpl() {}

/**
 * @copydoc Roo::Session::allocRooPC()
 */
Roo::unique_ptr<RooPC>
SessionImpl::allocRooPC()
{
    SpinLock::Lock lock_session(mutex);
    Proto::RooId rooId = Proto::RooId(
        sessionId, nextSequenceNumber.fetch_add(1, std::memory_order_relaxed));
    RooPCImpl* rpc = new RooPCImpl(this, rooId);
    rpcs.insert({rooId, rpc});
    return Roo::unique_ptr<RooPC>(rpc);
}

/**
 * @copydoc Roo::Session::receive()
 */
Roo::unique_ptr<ServerTask>
SessionImpl::receive()
{
    SpinLock::Lock lock_session(mutex);
    Roo::unique_ptr<ServerTask> task;
    if (!pendingTasks.empty()) {
        task = Roo::unique_ptr<ServerTask>(pendingTasks.front());
        pendingTasks.pop_front();
    }
    return task;
}

/**
 * @copydoc Roo::Session::poll()
 */
void
SessionImpl::poll()
{
    transport->poll();
    // Process incoming messages
    for (Homa::unique_ptr<Homa::InMessage> message = transport->receive();
         message; message = std::move(transport->receive())) {
        Proto::HeaderCommon common;
        message->get(0, &common, sizeof(common));
        if (common.opcode == Proto::Opcode::Message) {
            Proto::Message::Header header;
            message->get(0, &header, sizeof(header));
            if (header.type == Proto::Message::Type::Response) {
                // Incoming message is a response
                SpinLock::Lock lock_session(mutex);
                auto it = rpcs.find(header.rooId);
                if (it != rpcs.end()) {
                    RooPCImpl* rpc = it->second;
                    rpc->handleResponse(&header, std::move(message));
                } else {
                    // There is no RooPC waiting for this message.
                }
            } else {
                // Incoming message is a request.
                ServerTaskImpl* task =
                    new ServerTaskImpl(this, &header, std::move(message));
                pendingTasks.push_back(task);
            }
        } else if (common.opcode == Proto::Opcode::Delegation) {
            Proto::Delegation::Header header;
            message->get(0, &header, sizeof(header));
            SpinLock::Lock lock_session(mutex);
            auto it = rpcs.find(header.rooId);
            if (it != rpcs.end()) {
                RooPCImpl* rpc = it->second;
                rpc->handleDelegation(&header, std::move(message));
            } else {
                // There is no RooPC waiting for this message.
            }
        } else {
            WARNING("Unexpected protocol message received.");
        }
    }
    // Check detached ServerTasks
    {
        SpinLock::Lock lock_session(mutex);
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
            }
        }
    }
}

/**
 * Return a new unique RequestId.
 */
Proto::RequestId
SessionImpl::allocRequestId()
{
    return Proto::RequestId(
        sessionId, nextSequenceNumber.fetch_add(1, std::memory_order_relaxed));
}

/**
 * Discard a previously allocated RooPC.
 */
void
SessionImpl::dropRooPC(RooPCImpl* rpc)
{
    SpinLock::Lock lock_session(mutex);
    rpcs.erase(rpc->getId());
    delete rpc;
}

/**
 * Pass custody of a detached ServerTask to this session so that this session
 * can ensure its outbound message are completely sent.
 */
void
SessionImpl::remandTask(ServerTaskImpl* task)
{
    SpinLock::Lock lock_session(mutex);
    detachedTasks.push_back(task);
}

}  // namespace Roo
