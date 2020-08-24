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

#include "RooPCImpl.h"

#include "Debug.h"
#include "Perf.h"
#include "SocketImpl.h"

namespace Roo {

/**
 * RooPCImpl constructor.
 */
RooPCImpl::RooPCImpl(SocketImpl* socket, Proto::RooId rooId)
    : socket(socket)
    , rooId(rooId)
    , error(false)
    , requestCount(0)
    , pendingRequests()
    , responseQueue()
    , responses()
    , tasks()
    , manifestsOutstanding(0)
    , expectedResponses()
    , responsesOutstanding(0)
{}

/**
 * RooPCImpl destructor.
 */
RooPCImpl::~RooPCImpl() = default;

/**
 * @copydoc RooPCImpl::send()
 */
void
RooPCImpl::send(Homa::Driver::Address destination, const void* request,
                std::size_t length)
{
    Perf::Timer timer;
    SpinLock::Lock lock(mutex);
    Homa::unique_ptr<Homa::OutMessage> message = socket->transport->alloc();
    Homa::Driver::Address replyAddress =
        socket->transport->getDriver()->getLocalAddress();
    Proto::BranchId branchId(rooId, requestCount);
    requestCount += 1;
    Proto::RequestId requestId(branchId, 0);
    Proto::RequestHeader outboundHeader(rooId, requestId);
    socket->transport->getDriver()->addressToWireFormat(
        replyAddress, &outboundHeader.replyAddress);
    message->append(&outboundHeader, sizeof(outboundHeader));
    message->append(request, length);
    Perf::counters.tx_message_bytes.add(sizeof(outboundHeader) + length);

    // Track spawned tasks
    tasks.insert({branchId, {false, requestId, destination, 0}});
    manifestsOutstanding++;

    message->send(destination,
                  Homa::OutMessage::NO_RETRY | Homa::OutMessage::NO_KEEP_ALIVE);
    pendingRequests.push_back(std::move(message));
    Perf::counters.client_api_cycles.add(timer.split());
}

/**
 * @copydoc RooPCImpl::receive()
 */
Homa::InMessage*
RooPCImpl::receive()
{
    Perf::Timer timer;
    SpinLock::Lock lock(mutex);
    Homa::InMessage* response = nullptr;
    if (!responseQueue.empty()) {
        response = responseQueue.front();
        responseQueue.pop_front();
        Perf::counters.client_api_cycles.add(timer.split());
    }
    return response;
}

/**
 * @copydoc RooPCImpl::checkStatus()
 */
RooPC::Status
RooPCImpl::checkStatus()
{
    SpinLock::Lock lock(mutex);
    if (requestCount == 0) {
        return Status::NOT_STARTED;
    } else if (manifestsOutstanding == 0 && responsesOutstanding == 0) {
        return Status::COMPLETED;
    } else if (error) {
        return Status::FAILED;
    }

    // Check for failed requests
    for (auto it = pendingRequests.begin(); it != pendingRequests.end(); ++it) {
        Homa::OutMessage* request = it->get();
        if (request->getStatus() == Homa::OutMessage::Status::FAILED) {
            return Status::FAILED;
        }
    }

    return Status::IN_PROGRESS;
}

/**
 * @copydoc RooPCImpl::wait()
 */
void
RooPCImpl::wait()
{
    while (checkStatus() == Status::IN_PROGRESS) {
        socket->poll();
    }
}

/**
 * @copydoc RooPCImpl::destroy()
 */
void
RooPCImpl::destroy()
{
    Perf::Timer timer;
    // Don't actually free the object yet.  Return contol to the managing
    // socket so it can do some clean up.
    socket->dropRooPC(this);
    Perf::counters.client_api_cycles.add(timer.split());
}

/**
 * Add the incoming response message to this RooPC.
 *
 * @param header
 *      Preparsed header for the incoming response.
 * @param message
 *      The incoming response message to add.
 */
void
RooPCImpl::handleResponse(Proto::ResponseHeader* header,
                          Homa::unique_ptr<Homa::InMessage> message)
{
    SpinLock::Lock lock(mutex);
    message->strip(sizeof(Proto::ResponseHeader));

    // Process an implied manifiest if available.
    if (header->manifestImplied) {
        // Mark manifest received.
        markManifestReceived(header->branchId, lock);
    }

    // Process piggy-backed manifest if available.
    if (header->hasManifest) {
        processManifest(&header->manifest, lock);
    }

    // Process the incoming response message.
    auto ret = expectedResponses.insert({header->responseId, true});
    if (ret.second) {
        // New unanticipated response received.
        responseQueue.push_back(message.get());
        responses.push_back(std::move(message));
    } else if (ret.first->second == false) {
        // Expected response received.
        ret.first->second = true;
        responsesOutstanding--;
        responseQueue.push_back(message.get());
        responses.push_back(std::move(message));
    } else {
        // Response already received
        NOTICE("Duplicate response received for RooPC (%lu, %lu)",
               rooId.socketId, rooId.sequence);
    }
    if (manifestsOutstanding == 0 && responsesOutstanding == 0) {
        // RooPC is complete
        pendingRequests.clear();
    }
}

/**
 * Process an incoming Manifest message.
 *
 * @param header
 *      Parsed contents of the Manifest message.
 * @param message
 *      The incoming message containing the Manifest.
 */
void
RooPCImpl::handleManifest(Proto::ManifestHeader* header,
                          Homa::unique_ptr<Homa::InMessage> message)
{
    SpinLock::Lock lock(mutex);
    std::size_t offest = sizeof(Proto::ManifestHeader);
    for (size_t i = 0; i < header->manifestCount; ++i) {
        Proto::Manifest manifest;
        message->get(offest + (sizeof(Proto::Manifest) * i), &manifest,
                     sizeof(Proto::Manifest));
        processManifest(&manifest, lock);
    }

    if (manifestsOutstanding == 0 && responsesOutstanding == 0) {
        // RooPC is complete
        pendingRequests.clear();
    }
}

/**
 * Process an incoming Pong message.
 *
 * @param header
 *      Parsed contents of the Pong message.
 * @param message
 *      The incoming message containing the Pong.
 */
void
RooPCImpl::handlePong(Proto::PongHeader* header,
                      Homa::unique_ptr<Homa::InMessage> message)
{
    SpinLock::Lock lock(mutex);
    (void)message;
    if (header->branchComplete) {
        processManifest(&header->manifest, lock);
    } else {
        Proto::RequestId requestId = header->manifest.requestId;
        auto it = tasks.find(requestId.branchId);
        if (it != tasks.end()) {
            TaskInfo* task = &it->second;
            if (!(task->pingReceiverId.branchId == requestId.branchId) ||
                task->pingReceiverId.sequence < requestId.sequence) {
                // Update the ping target
                task->pingReceiverId = requestId;
                task->pingAddress = socket->transport->getDriver()->getAddress(
                    &header->manifest.serverAddress);
            }
            task->pingCount = 0;
        }
    }

    if (manifestsOutstanding == 0 && responsesOutstanding == 0) {
        // RooPC is complete
        pendingRequests.clear();
    }
}

/**
 * Process an incoming Error message.
 *
 * @param header
 *      Parsed contents of the Error message.
 * @param message
 *      The incoming message containing the Error.
 */
void
RooPCImpl::handleError(Proto::ErrorHeader* header,
                       Homa::unique_ptr<Homa::InMessage> message)
{
    SpinLock::Lock lock(mutex);
    (void)header;
    (void)message;
    error = true;
}

/**
 * Handle a timeout event.
 *
 * @return
 *      True if the RooPC timeout should be reset; false, otherwise.
 */
bool
RooPCImpl::handleTimeout()
{
    SpinLock::Lock lock(mutex);
    if (manifestsOutstanding > 0) {
        // Ping tasks for which we don't have manifests.
        for (auto& it : tasks) {
            Proto::BranchId targetBranch = it.first;
            TaskInfo* info = &it.second;

            // Check if task is still in progress.
            if (info->complete) {
                // nothing to do
                continue;
            }

            // Check if task has timedout
            if (info->pingCount > 3) {
                error = true;
                return false;
            }

            // Send a ping.
            Homa::unique_ptr<Homa::OutMessage> ping =
                socket->transport->alloc();
            Proto::PingHeader pingHeader(info->pingReceiverId, targetBranch,
                                         true);
            ping->append(&pingHeader, sizeof(Proto::PingHeader));
            ping->send(info->pingAddress, Homa::OutMessage::NO_RETRY |
                                              Homa::OutMessage::NO_KEEP_ALIVE);
            info->pingCount++;
        }
        return true;
    } else if (responsesOutstanding > 0) {
        // All task are complete but the responses haven't come in yet.
        // Consider the responses lost and generate an error.
        error = true;
        return false;
    } else {
        // All manifests and responses have been received.
        return false;
    }
}

/**
 * Helper method that performs the necessary book-keeping to mark a request's
 * manifest as received.
 *
 * @param branchId
 *      Identifies the request associated with the received manifest.
 * @param lock
 *      Reminds the caller that the RooPCImpl::mutex should be held.
 */
void
RooPCImpl::markManifestReceived(Proto::BranchId branchId,
                                const SpinLock::Lock& lock)
{
    (void)lock;

    auto checkTask = tasks.insert({branchId, {true, {}, {}, 0}});
    if (checkTask.second) {
        // Task not previously tracked.
        // Nothing to do;
    } else if (checkTask.first->second.complete == false) {
        // Task previously tracked.
        checkTask.first->second.complete = true;
        manifestsOutstanding--;
    } else {
        // Manifest previously received.
        WARNING("Duplicate Manifest for RooPC (%lu, %lu)", rooId.socketId,
                rooId.sequence);
    }
}

/**
 * Helper method that executes the Manifest related parts of a RooPC's
 * completion tracking logic; should be call for each received Manifest.
 *
 * @param manifest
 *      The received Manifest that should be processed.
 * @param lock
 *      Reminds the caller that the RooPCImpl::mutex should be held.
 */
void
RooPCImpl::processManifest(Proto::Manifest* manifest,
                           const SpinLock::Lock& lock)
{
    (void)lock;

    Homa::Driver::Address taskServerAddress =
        socket->transport->getDriver()->getAddress(&manifest->serverAddress);

    // Add tracked task branches.
    for (uint64_t i = 0; i < manifest->requestCount; ++i) {
        Proto::BranchId branchId(manifest->taskId, i);
        auto checkTask = tasks.insert(
            {branchId, {false, manifest->requestId, taskServerAddress, 0}});
        if (checkTask.second) {
            // Task not previously tracked.
            manifestsOutstanding++;
        }
    }

    // Add expected responses.
    for (uint64_t i = 0; i < manifest->responseCount; ++i) {
        Proto::ResponseId responseId(manifest->taskId, i);
        auto checkResponse = expectedResponses.insert({responseId, false});
        if (checkResponse.second) {
            // Response not yet tracked.
            responsesOutstanding++;
        }
    }

    // Mark manifest received.
    markManifestReceived(manifest->requestId.branchId, lock);
}

}  // namespace Roo
