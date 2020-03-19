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
    , mutex()
    , nextRooSequenceNumber(1)
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
    Proto::RooId rooId =
        Proto::RooId(transport->getId(), nextRooSequenceNumber++);
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
    for (Homa::InMessage* message = transport->receive(); message != nullptr;
         message = transport->receive()) {
        Proto::Message::Header header;
        message->get(0, &header, sizeof(header));
        message->strip(sizeof(header));
        if (header.stageId == Proto::Message::ULTIMATE_RESPONSE_ID) {
            // Incoming message is a response
            SpinLock::Lock lock_session(mutex);
            auto it = rpcs.find(header.rooId);
            if (it != rpcs.end()) {
                RooPCImpl* rpc = it->second;
                rpc->queueResponse(message);
            } else {
                // There is no RooPC waiting for this message; Drop it.
                message->release();
            }
        } else {
            // Incoming message is a request.
            ServerTaskImpl* task = new ServerTaskImpl(this, &header, message);
            pendingTasks.push_back(task);
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
