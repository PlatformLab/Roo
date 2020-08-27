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

using PerfUtils::Cycles;

// Basic timeout unit.
const uint64_t BASE_TIMEOUT_US{2000};
/// Microseconds to wait before pinging to check on requests.
const uint64_t WORRY_TIMEOUT_US{BASE_TIMEOUT_US};
/// Microseconds of inactive before garbage collecting a task.
const uint64_t TASK_TIMEOUT_US{3 * BASE_TIMEOUT_US};

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
    , WORRY_TIMEOUT_CYCLES(Cycles::fromMicroseconds(WORRY_TIMEOUT_US))
    , TASK_TIMEOUT_CYCLES(Cycles::fromMicroseconds(TASK_TIMEOUT_US))
    , mutex()
    , rpcs()
    , rpcTimeouts()
    , nextRpcTimeout(UINT64_MAX)
    , tasks()
    , pendingTasks()
    , detachedTasks()
    , taskTimeouts()
    , nextTaskTimeout(UINT64_MAX)
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
    Perf::Timer timer;
    SpinLock::Lock lock_socket(mutex);
    Proto::RooId rooId = allocTaskId();
    RooPCImpl* rpc = new RooPCImpl(this, rooId);
    rpcs.insert({rooId, rpc});
    uint64_t timeoutTime = Cycles::rdtsc() + WORRY_TIMEOUT_CYCLES;
    rpcTimeouts.push_back({timeoutTime, rooId});
    nextRpcTimeout.store(rpcTimeouts.front().expirationTime,
                         std::memory_order_relaxed);
    Perf::counters.client_api_cycles.add(timer.split());
    return Roo::unique_ptr<RooPC>(rpc);
}

/**
 * @copydoc Roo::Socket::receive()
 */
Roo::unique_ptr<ServerTask>
SocketImpl::receive()
{
    Perf::Timer timer;
    SpinLock::Lock lock_socket(mutex);
    Roo::unique_ptr<ServerTask> task;
    if (!pendingTasks.empty()) {
        task = Roo::unique_ptr<ServerTask>(pendingTasks.front());
        pendingTasks.pop_front();
        Perf::counters.server_api_cycles.add(timer.split());
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

    Perf::Timer timer;
    processIncomingMessages();
    checkDetachedTasks();
    checkClientTimeouts();
    checkTaskTimeouts();
    Perf::counters.poll_total_cycles.add(timer.split());
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
        Perf::counters.poll_active_cycles.add(activityTimer.split());
    }
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

    SpinLock::Lock lock_socket(mutex);
    auto it = detachedTasks.begin();
    while (it != detachedTasks.end()) {
        ServerTaskImpl* task = *it;
        bool not_done = task->poll();
        activityTimer.split();
        if (not_done) {
            ++it;
        } else {
            // ServerTask is done polling
            it = detachedTasks.erase(it);
            uint64_t timeoutTime = Cycles::rdtsc() + TASK_TIMEOUT_CYCLES;
            taskTimeouts.push_back({timeoutTime, task});
            nextTaskTimeout.store(taskTimeouts.front().expirationTime,
                                  std::memory_order_relaxed);
            Perf::counters.poll_active_cycles.add(activityTimer.split());
        }
    }
}

/**
 * Process any expired RooPC timeouts; seperated out of poll() for testing.
 */
void
SocketImpl::checkClientTimeouts()
{
    // Keep track of time spent doing active processing versus idle.
    Perf::Timer activityTimer;

    // Avoid calling rdtsc() again and use the activityTimer time instead.
    uint64_t now = activityTimer.read();

    // Fast path check if there are any timeouts about to expire.
    if (now < nextRpcTimeout.load(std::memory_order_relaxed)) {
        return;
    }

    SpinLock::Lock lock_socket(mutex);
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
                    uint64_t timeoutTime =
                        Cycles::rdtsc() + WORRY_TIMEOUT_CYCLES;
                    rpcTimeouts.push_back({timeoutTime, rooId});
                }
            }
            Perf::counters.poll_active_cycles.add(activityTimer.split());
        }
    }

    if (rpcTimeouts.empty()) {
        nextRpcTimeout.store(UINT64_MAX, std::memory_order_relaxed);
    } else {
        nextRpcTimeout.store(rpcTimeouts.front().expirationTime,
                             std::memory_order_relaxed);
    }
}

/**
 * Process any expired task timeouts; seperated out of poll() for testing.
 */
void
SocketImpl::checkTaskTimeouts()
{
    // Keep track of time spent doing active processing versus idle.
    Perf::Timer activityTimer;

    // Avoid calling rdtsc() again and use the activityTimer time instead.
    uint64_t now = activityTimer.read();

    // Fast path check if there are any timeouts about to expire.
    if (now < nextTaskTimeout.load(std::memory_order_relaxed)) {
        return;
    }

    SpinLock::Lock lock_socket(mutex);
    auto it = taskTimeouts.begin();
    while (it != taskTimeouts.end()) {
        if (now < it->expirationTime) {
            break;
        } else {
            ServerTaskImpl* task = it->object;
            it = taskTimeouts.erase(it);
            if (task->handleTimeout()) {
                // Timeout handled and reset
                uint64_t timeoutTime = Cycles::rdtsc() + TASK_TIMEOUT_CYCLES;
                taskTimeouts.push_back({timeoutTime, task});
            } else {
                tasks.erase(task->getRequestId());
                delete task;
            }
            Perf::counters.poll_active_cycles.add(activityTimer.split());
        }
    }

    if (taskTimeouts.empty()) {
        nextTaskTimeout.store(UINT64_MAX, std::memory_order_relaxed);
    } else {
        nextTaskTimeout.store(taskTimeouts.front().expirationTime,
                              std::memory_order_relaxed);
    }
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
