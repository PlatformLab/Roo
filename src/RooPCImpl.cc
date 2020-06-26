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
    , requestCount(0)
    , pendingRequests()
    , responseQueue()
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
 * @copydoc RooPCImpl::allocRequest()
 */
Homa::unique_ptr<Homa::OutMessage>
RooPCImpl::allocRequest()
{
    Homa::unique_ptr<Homa::OutMessage> message = socket->transport->alloc();
    message->reserve(sizeof(Proto::RequestHeader));
    return message;
}

/**
 * @copydoc RooPCImpl::send()
 */
void
RooPCImpl::send(Homa::Driver::Address destination,
                Homa::unique_ptr<Homa::OutMessage> request)
{
    SpinLock::Lock lock(mutex);
    Perf::counters.tx_message_bytes.add(request->length());
    Homa::Driver::Address replyAddress =
        socket->transport->getDriver()->getLocalAddress();
    Proto::BranchId branchId(rooId, requestCount);
    requestCount += 1;
    Proto::RequestHeader outboundHeader(rooId, branchId, true);
    socket->transport->getDriver()->addressToWireFormat(
        replyAddress, &outboundHeader.replyAddress);
    request->prepend(&outboundHeader, sizeof(outboundHeader));

    // Track spawned tasks
    tasks.insert({branchId, false});
    manifestsOutstanding++;

    request->send(destination, Homa::OutMessage::NO_RETRY);
    pendingRequests.push_back(std::move(request));
}

/**
 * @copydoc RooPCImpl::receive()
 */
Homa::unique_ptr<Homa::InMessage>
RooPCImpl::receive()
{
    SpinLock::Lock lock(mutex);
    Homa::unique_ptr<Homa::InMessage> response = nullptr;
    if (!responseQueue.empty()) {
        response = std::move(responseQueue.front());
        responseQueue.pop_front();
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
    // Don't actually free the object yet.  Return contol to the managing
    // socket so it can do some clean up.
    socket->dropRooPC(this);
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
    message->acknowledge();
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
        // Make sure a new task is tracked if it isn't already.
        auto checkTask = tasks.insert({header->branchId, false});
        if (checkTask.second) {
            // Task not previously tracked.
            manifestsOutstanding++;
        }
        responseQueue.push_back(std::move(message));
    } else if (ret.first->second == false) {
        // Expected response received.
        ret.first->second = true;
        responsesOutstanding--;
        responseQueue.push_back(std::move(message));
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
    size_t offest = sizeof(Proto::ManifestHeader);
    for (size_t i = 0; i < header->manifestCount; ++i) {
        Proto::Manifest manifest;
        message->get(offest + (sizeof(Proto::Manifest) * i), &manifest,
                     sizeof(Proto::Manifest));
        processManifest(&manifest, lock);
    }
    message->acknowledge();

    if (manifestsOutstanding == 0 && responsesOutstanding == 0) {
        // RooPC is complete
        pendingRequests.clear();
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

    auto checkTask = tasks.insert({branchId, true});
    if (checkTask.second) {
        // Task not previously tracked.
        // Nothing to do;
    } else if (checkTask.first->second == false) {
        // Task previously tracked.
        checkTask.first->second = true;
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

    // Add tracked task branches.
    for (uint64_t i = 0; i < manifest->requestCount; ++i) {
        Proto::BranchId branchId(manifest->taskId, i);
        auto checkTask = tasks.insert({branchId, false});
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
    markManifestReceived(manifest->branchId, lock);
}

}  // namespace Roo
