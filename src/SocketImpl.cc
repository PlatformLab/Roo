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

// Basic timeout unit.
const std::chrono::microseconds BASE_TIMEOUT_US{2000};
/// Microseconds to wait before pinging to check on requests.
const std::chrono::microseconds WORRY_TIMEOUT_US{BASE_TIMEOUT_US};
/// Microseconds to wait before performing retires on inbound messages.
const std::chrono::microseconds TASK_TIMEOUT_US{3 * BASE_TIMEOUT_US};

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
    , tasks()
    , pendingTasks()
    , detachedTasks()
    , taskTimeouts()
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
    std::chrono::steady_clock::time_point timeoutTime =
        std::chrono::steady_clock::now() + WORRY_TIMEOUT_US;
    rpcTimeouts.push_back({timeoutTime, rooId});
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
    // Let the transport make incremental progress.
    transport->poll();
    processIncomingMessages();
    checkDetachedTasks();
    checkClientTimeouts();
    checkTaskTimeouts();
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
 * Check and dispatch any incoming messages; separated from poll() for testing.
 */
void
SocketImpl::processIncomingMessages()
{
    // Keep track of time spent doing active processing versus idle.
    Perf::Timer activityTimer;
    activityTimer.split();
    uint64_t activeTime = 0;
    uint64_t idleTime = 0;

    // Process incoming messages
    for (Homa::unique_ptr<Homa::InMessage> message = transport->receive();
         message; message = std::move(transport->receive())) {
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
            tasks.insert({task->getRequestId(), task});
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
            Proto::ManifestHeader manifest;
            message->get(0, &manifest, sizeof(manifest));
            SpinLock::Lock lock_socket(mutex);
            auto it = rpcs.find(manifest.rooId);
            if (it != rpcs.end()) {
                RooPCImpl* rpc = it->second;
                rpc->handleManifest(&manifest, std::move(message));
            } else {
                // There is no RooPC waiting for this manifest.
            }
        } else if (common.opcode == Proto::Opcode::Ping) {
            Proto::PingHeader header;
            message->get(0, &header, sizeof(header));
            SpinLock::Lock lock_socket(mutex);
            auto it = tasks.find(header.requestId);
            if (it != tasks.end()) {
                ServerTaskImpl* task = it->second;
                task->handlePing(&header, std::move(message));
            } else {
                // There is no associated active ServerTask.
            }
        } else if (common.opcode == Proto::Opcode::Pong) {
            Proto::PongHeader header;
            message->get(0, &header, sizeof(header));
            SpinLock::Lock lock_socket(mutex);
            auto it = rpcs.find(header.rooId);
            if (it != rpcs.end()) {
                RooPCImpl* rpc = it->second;
                rpc->handlePong(&header, std::move(message));
            } else {
                // There is no RooPC waiting for this message.
            }
        } else if (common.opcode == Proto::Opcode::Error) {
            Proto::ErrorHeader header;
            message->get(0, &header, sizeof(header));
            SpinLock::Lock lock_socket(mutex);
            auto it = rpcs.find(header.rooId);
            if (it != rpcs.end()) {
                RooPCImpl* rpc = it->second;
                rpc->handleError(&header, std::move(message));
            } else {
                // There is no RooPC waiting for this message.
            }
        } else {
            WARNING("Unexpected protocol message received.");
        }
        activeTime += activityTimer.split();
    }
    idleTime += activityTimer.split();

    Perf::counters.active_cycles.add(activeTime);
    Perf::counters.idle_cycles.add(idleTime);
}

/**
 * Check on tasks that have been processed by the application but has not yet
 * finished transmitting all its messages; seperated out of poll() for testing.
 */
void
SocketImpl::checkDetachedTasks()
{
    // Keep track of time spent doing active processing versus idle.
    Perf::Timer activityTimer;
    activityTimer.split();
    uint64_t activeTime = 0;
    uint64_t idleTime = 0;

    SpinLock::Lock lock_socket(mutex);
    auto it = detachedTasks.begin();
    while (it != detachedTasks.end()) {
        ServerTaskImpl* task = *it;
        idleTime += activityTimer.split();
        bool not_done = task->poll();
        activityTimer.split();
        if (not_done) {
            ++it;
        } else {
            // ServerTask is done polling
            it = detachedTasks.erase(it);
            std::chrono::steady_clock::time_point timeoutTime =
                std::chrono::steady_clock::now() + TASK_TIMEOUT_US;
            taskTimeouts.push_back({timeoutTime, task});
            activeTime += activityTimer.split();
        }
    }
    idleTime += activityTimer.split();

    Perf::counters.active_cycles.add(activeTime);
    Perf::counters.idle_cycles.add(idleTime);
}

/**
 * Process any expired RooPC timeouts; seperated out of poll() for testing.
 */
void
SocketImpl::checkClientTimeouts()
{
    // Keep track of time spent doing active processing versus idle.
    Perf::Timer activityTimer;
    activityTimer.split();
    uint64_t activeTime = 0;
    uint64_t idleTime = 0;

    SpinLock::Lock lock_socket(mutex);
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    auto it = rpcTimeouts.begin();
    while (it != rpcTimeouts.end()) {
        if (now < it->expirationTime) {
            break;
        } else {
            Proto::RooId rooId = it->object;
            it = rpcTimeouts.erase(it);
            auto rpcHandle = rpcs.find(rooId);
            if (rpcHandle != rpcs.end()) {
                RooPCImpl* rpc = rpcHandle->second;
                if (rpc->handleTimeout()) {
                    // Timeout handled and reset
                    std::chrono::steady_clock::time_point timeoutTime =
                        std::chrono::steady_clock::now() + WORRY_TIMEOUT_US;
                    rpcTimeouts.push_back({timeoutTime, rooId});
                }
            }
            activeTime += activityTimer.split();
        }
    }
    idleTime += activityTimer.split();

    Perf::counters.active_cycles.add(activeTime);
    Perf::counters.idle_cycles.add(idleTime);
}

/**
 * Process any expired task timeouts; seperated out of poll() for testing.
 */
void
SocketImpl::checkTaskTimeouts()
{
    // Keep track of time spent doing active processing versus idle.
    Perf::Timer activityTimer;
    activityTimer.split();
    uint64_t activeTime = 0;
    uint64_t idleTime = 0;

    SpinLock::Lock lock_socket(mutex);
    std::chrono::steady_clock::time_point now =
        std::chrono::steady_clock::now();
    auto it = taskTimeouts.begin();
    while (it != taskTimeouts.end()) {
        if (now < it->expirationTime) {
            break;
        } else {
            ServerTaskImpl* task = it->object;
            it = taskTimeouts.erase(it);
            if (task->handleTimeout()) {
                // Timeout handled and reset
                std::chrono::steady_clock::time_point timeoutTime =
                    std::chrono::steady_clock::now() + TASK_TIMEOUT_US;
                taskTimeouts.push_back({timeoutTime, task});
            } else {
                tasks.erase(task->getRequestId());
                delete task;
            }
            activeTime += activityTimer.split();
        }
    }
    idleTime += activityTimer.split();

    Perf::counters.active_cycles.add(activeTime);
    Perf::counters.idle_cycles.add(idleTime);
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
