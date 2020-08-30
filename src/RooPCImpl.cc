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

#include "ControlMessage.h"
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
    , responseQueue()
    , responses()
    , branches()
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

    // Track spawned branches
    branches.insert({branchId, {false, requestId, destination, 0}});
    manifestsOutstanding++;

    message->send(destination,
                  Homa::OutMessage::NO_RETRY | Homa::OutMessage::NO_KEEP_ALIVE);
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
    } else {
        return Status::IN_PROGRESS;
    }
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
        updateBranchInfo(header->branchId, true, {}, {}, lock);
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
    message->strip(sizeof(Proto::PongHeader));
    const Proto::RequestId requestId = header->requestId;

    auto it = branches.find(requestId.branchId);
    if (it == branches.end()) {
        WARNING("Unexpected PONG from RequestId %s; PONG dropped.",
                requestId.toString().c_str());
        return;
    }

    BranchInfo* branch = &it->second;
    branch->pingTimeouts = 0;

    if (header->taskComplete && !header->branchComplete) {
        // Special case where the task only has a single delegated request.
        assert(header->requestCount == 1 && header->responseCount == 0);
        if (branch->complete) {
            // Stale pong received.
        } else {
            const Proto::RequestId pingRequestId{requestId.branchId,
                                                 requestId.sequence + 1};
            Homa::Driver::WireFormatAddress rawAddress;
            message->get(0, &rawAddress, sizeof(rawAddress));
            message->strip(sizeof(rawAddress));
            const Homa::Driver::Address pingAddress =
                socket->transport->getDriver()->getAddress(&rawAddress);
            const bool updated = branch->updatePingTarget(
                requestId.branchId, pingRequestId, pingAddress);
            if (updated) {
                assert(!branch->complete);
                // Send ping to updated ping target
                ControlMessage::send<Proto::PingHeader>(socket->transport,
                                                        branch->pingAddress,
                                                        branch->pingReceiverId);
            }
        }
    } else {
        // Normal case

        // Update branch info
        if (branch->complete) {
            // Nothing to do.
        } else if (header->branchComplete) {
            // Mark branch complete.
            branch->complete = true;
            manifestsOutstanding--;
        }

        // Update child branches
        for (uint32_t i = 0; i < header->requestCount; ++i) {
            const Proto::RequestId childRequestId{{header->taskId, i}, 0};
            Homa::Driver::WireFormatAddress rawAddress;
            message->get(0, &rawAddress, sizeof(rawAddress));
            message->strip(sizeof(rawAddress));
            const Homa::Driver::Address pingAddress =
                socket->transport->getDriver()->getAddress(&rawAddress);
            auto ret = updateBranchInfo(childRequestId.branchId, false,
                                        childRequestId, pingAddress, lock);
            BranchInfo* const info = ret.first;
            if (ret.second) {
                // Send ping to updated ping target
                ControlMessage::send<Proto::PingHeader>(
                    socket->transport, info->pingAddress, info->pingReceiverId);
            }
        }

        // Update expected responses.
        for (uint32_t i = 0; i < header->responseCount; ++i) {
            Proto::ResponseId responseId(header->taskId, i);
            auto checkResponse = expectedResponses.insert({responseId, false});
            if (checkResponse.second) {
                // Response not yet tracked.
                responsesOutstanding++;
            }
        }
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
        // Ping branches for which we don't have manifests.

        std::unordered_map<Proto::RequestId, Homa::Driver::Address,
                           Proto::RequestId::Hasher>
            pingCandidates;
        for (auto& it : branches) {
            BranchInfo* info = &it.second;

            // Check if the branch is still in progress.
            if (info->complete) {
                // nothing to do
                continue;
            }

            // Check if the branch has timed out
            if (info->pingTimeouts > 3) {
                error = true;
                return false;
            }

            // Add to the list of pings that need to be sent.
            pingCandidates.insert({info->pingReceiverId, info->pingAddress});
            info->pingTimeouts++;
        }

        // Send out the pings
        for (auto& it : pingCandidates) {
            ControlMessage::send<Proto::PingHeader>(socket->transport,
                                                    it.second, it.first);
        }

        return true;
    } else if (responsesOutstanding > 0) {
        // All tasks are complete but the responses haven't come in yet.
        // Consider the responses lost and generate an error.
        error = true;
        return false;
    } else {
        // All manifests and responses have been received.
        return false;
    }
}

/**
 * Helper method to update information about where pings for this branch should
 * be sent with the provided information if the provided information is more
 * up-to-date.
 *
 * @param branchId
 *      Id of the branch being updated.
 * @param updatedId
 *      The pingReceiverId should take on this value if this value is more
 *      up-to-date.
 * @param updatedAddress
 *      The pingAddress should take on this value if this value is more
 *      up-to-date.
 *
 */
bool
RooPCImpl::BranchInfo::updatePingTarget(Proto::BranchId branchId,
                                        Proto::RequestId updatedId,
                                        Homa::Driver::Address updatedAddress)
{
    // Check if the ping target needs to be updated.
    // This can occur if:
    //   1) The branch entry was first created when a manifest was
    //      received; manifests don't contain the child branch address so
    //      the branch entry is initialized with the parent branch address
    //      was used instead (i.e. branchId's don't match).
    //   2) The branch is being succeeded by a branch in the same branch
    //      (i.e. branchIds match and new sequence number is larger).
    if (updatedId.branchId == branchId) {
        if (!(pingReceiverId.branchId == branchId) ||
            (updatedId.sequence > pingReceiverId.sequence)) {
            // Update the ping target
            pingReceiverId = updatedId;
            pingAddress = updatedAddress;
            return true;
        } else {
            // Nothing to do. The information that has just arrive is
            // either redundant or represents information from for a
            // parent branch.
        }
    } else {
        // Nothing to do. The "updated" ping target is for a different
        // branch (e.g. the parent branch). We either already have this
        // parent branch information or have information directly about
        // the branch itself.
    }
    return false;
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
        updateBranchInfo(branchId, false, manifest->requestId,
                         taskServerAddress, lock);
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
    updateBranchInfo(manifest->requestId.branchId, true, {}, {}, lock);
}

/**
 * Helper method to add/update the tracked branch information to relect the
 * provided information if the information is more up-to-date. If stale
 * information is provided, this method does nothing.
 *
 * @param branchId
 *      Id of the branch that should be added/updated.
 * @param isComplete
 *      Indicates whether the branch has finished processing.
 * @param pingReceiverId
 *      RequestId of the task that can be pinged to check on the branch.
 * @param pingAddress
 *      Accompanying address to which pings can be sent.
 * @param lock
 *      Reminds the caller that the RooPCImpl::mutex should be held.
 */
std::pair<RooPCImpl::BranchInfo*, bool>
RooPCImpl::updateBranchInfo(Proto::BranchId branchId, bool isComplete,
                            Proto::RequestId pingReceiverId,
                            Homa::Driver::Address pingAddress,
                            const SpinLock::Lock& lock)
{
    (void)lock;
    bool branchUpdated = false;
    auto ret = branches.insert(
        {branchId, {isComplete, pingReceiverId, pingAddress, 0}});
    BranchInfo* const branch = &ret.first->second;
    if (ret.second) {
        // Branch not previously tracked.
        if (!isComplete) {
            manifestsOutstanding++;
        }
        branchUpdated = true;
    } else {
        // Needs updating
        if (branch->complete) {
            // Nothing to do.
        } else if (isComplete) {
            // Mark branch complete.
            branch->complete = true;
            manifestsOutstanding--;
            branchUpdated = true;
        } else {
            branchUpdated =
                branch->updatePingTarget(branchId, pingReceiverId, pingAddress);
        }
    }
    return std::make_pair(branch, branchUpdated);
}

}  // namespace Roo
