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

#include "ServerTaskImpl.h"

#include "Debug.h"
#include "Perf.h"
#include "SocketImpl.h"

namespace Roo {

/**
 * ServerTaskImpl constructor.
 *
 * @param socket
 *      Socket managing this ServerTask.
 * @param taskId
 *      Unique identifer assigned to this task.
 * @param requestHeader
 *      Contents of the request header.  This constructor does not take
 *      ownership of this pointer.
 * @param request
 *      The request message associated with this ServerTask.
 */
ServerTaskImpl::ServerTaskImpl(SocketImpl* socket, Proto::TaskId taskId,
                               Proto::RequestHeader const* requestHeader,
                               Homa::unique_ptr<Homa::InMessage> request)
    : detached(false)
    , socket(socket)
    , rooId(requestHeader->rooId)
    , requestId(requestHeader->requestId)
    , taskId(taskId)
    , request(std::move(request))
    , replyAddress(socket->transport->getDriver()->getAddress(
          &requestHeader->replyAddress))
    , responseCount(0)
    , requestCount(0)
    , outboundMessages()
    , pendingMessages()
    , pingInfo()
    , bufferedMessageIsRequest(false)
    , bufferedMessageAddress()
    , bufferedRequestHeader()
    , bufferedResponseHeader()
    , bufferedMessage()
    , hasUnsentManifest(requestHeader->hasManifest)
    , delegatedManifest(requestHeader->manifest)
{
    this->request->strip(sizeof(Proto::RequestHeader));
}

/**
 * ServerTaskImpl destructor.
 */
ServerTaskImpl::~ServerTaskImpl() = default;

/**
 * @copydoc ServerTask::getRequest()
 */
Homa::InMessage*
ServerTaskImpl::getRequest()
{
    return request.get();
}

/**
 * @copydoc ServerTask::reply()
 */
void
ServerTaskImpl::reply(const void* response, std::size_t length)
{
    Perf::Timer timer;

    // The ServerTask always buffers that last outbound message so that any
    // necessary manifest information can be piggy-back on the last message.
    // The response message provided in this call will be sent when the server
    // next calls either reply() or delegate() or when server is done processing
    // the ServerTask.

    // Send out any previously buffered message
    sendBufferedMessage();

    // Format the response message
    bufferedMessageIsRequest = false;
    Homa::unique_ptr<Homa::OutMessage> message = socket->transport->alloc();
    message->reserve(sizeof(Proto::ResponseHeader));
    message->append(response, length);
    new (&bufferedResponseHeader) Proto::ResponseHeader(
        rooId, requestId.branchId, Proto::ResponseId(taskId, responseCount));
    responseCount += 1;
    if (hasUnsentManifest) {
        // piggy-back the delegated manifest
        bufferedResponseHeader.hasManifest = true;
        bufferedResponseHeader.manifest = delegatedManifest;
        hasUnsentManifest = false;
    }
    bufferedMessageAddress = replyAddress;
    bufferedMessage = std::move(message);
    Perf::counters.server_api_cycles.add(timer.split());
}

/**
 * @copydoc ServerTask::delegate()
 */
void
ServerTaskImpl::delegate(Homa::Driver::Address destination, const void* request,
                         std::size_t length)
{
    Perf::Timer timer;

    // The ServerTask always buffers that last outbound message so that any
    // necessary manifest information can be piggy-back on the last message.
    // The request message provided in this call will be sent when the server
    // next calls either reply() or delegate() or when server is done processing
    // the ServerTask.

    // Send out any previously buffered message
    sendBufferedMessage();

    // Format the delegated request message
    bufferedMessageIsRequest = true;
    Homa::unique_ptr<Homa::OutMessage> message = socket->transport->alloc();
    message->reserve(sizeof(Proto::RequestHeader));
    message->append(request, length);
    Proto::BranchId newBranchId(taskId, requestCount);
    requestCount += 1;
    Proto::RequestId newRequestId(newBranchId, 0);
    new (&bufferedRequestHeader) Proto::RequestHeader(rooId, newRequestId);
    socket->transport->getDriver()->addressToWireFormat(
        replyAddress, &bufferedRequestHeader.replyAddress);
    if (hasUnsentManifest) {
        // piggy-back the delegated manifest
        bufferedRequestHeader.hasManifest = true;
        bufferedRequestHeader.manifest = delegatedManifest;
        hasUnsentManifest = false;
    }
    bufferedMessageAddress = destination;
    bufferedMessage = std::move(message);

    SpinLock::Lock lock(pingInfo.mutex);
    pingInfo.requests.push_back({newRequestId, destination});
    Perf::counters.server_api_cycles.add(timer.split());
}

/**
 * Perform an incremental amount of any necessary background processing.
 *
 * @return
 *      True, if more background processing is needed (i.e. poll needs to be
 *      called again). False, otherwise.
 */
bool
ServerTaskImpl::poll()
{
    // Keep track of time spent doing active processing versus idle.
    Perf::Timer timer;

    bool isInProgress = true;

    if (request->dropped()) {
        // Nothing left to do
        isInProgress = false;
        Perf::counters.poll_active_cycles.add(timer.split());
    } else if (pendingMessages.empty()) {
        // No more pending messages.
        isInProgress = false;
        Perf::counters.poll_active_cycles.add(timer.split());
    } else {
        // Check for any remaining pending messages
        auto it = pendingMessages.begin();
        while (it != pendingMessages.end()) {
            Homa::OutMessage::Status status = (*it)->getStatus();
            if (status == Homa::OutMessage::Status::SENT) {
                // Remove and keep checking for other pendingRequests
                it = pendingMessages.erase(it);
                Perf::counters.poll_active_cycles.add(timer.split());
            } else if (status == Homa::OutMessage::Status::FAILED) {
                // Send Error notification to Client
                Homa::unique_ptr<Homa::OutMessage> message =
                    socket->transport->alloc();
                Proto::ErrorHeader header(rooId);
                message->append(&header, sizeof(Proto::ErrorHeader));
                message->send(replyAddress,
                              Homa::OutMessage::NO_RETRY |
                                  Homa::OutMessage::NO_KEEP_ALIVE);
                // Failed, no need to keep checking
                isInProgress = false;
                Perf::counters.poll_active_cycles.add(timer.split());
                break;
            } else {
                ++it;
            }
        }
    }

    return isInProgress;
}

/**
 * Process an incoming Ping message.
 *
 * @param header
 *      Parsed contents of the Ping message.
 * @param message
 *      The incoming message containing the Ping.
 */
void
ServerTaskImpl::handlePing(Proto::PingHeader* header,
                           Homa::unique_ptr<Homa::InMessage> message)
{
    (void)header;
    (void)message;

    SpinLock::Lock lock(pingInfo.mutex);
    pingInfo.pingCount++;

    // Forward Pings
    for (auto& it : pingInfo.requests) {
        Homa::unique_ptr<Homa::OutMessage> ping = socket->transport->alloc();
        Proto::PingHeader pingHeader(it.requestId);
        ping->append(&pingHeader, sizeof(Proto::PingHeader));
        ping->send(it.destination, Homa::OutMessage::NO_RETRY |
                                       Homa::OutMessage::NO_KEEP_ALIVE);
    }

    // Reply with Pong
    Proto::PongHeader pongHeader(rooId, false);
    if (detached) {
        // Task is done processing; check if this task terminate this branch.
        if (requestCount + responseCount != 1 || responseCount == 1) {
            // Branch has terminated
            pongHeader.branchComplete = true;
        } else {
            // Task is generated a single delegated request so the branch is
            // not complete; pongHeader.branchComplete = false;
        }
        new (&pongHeader.manifest)
            Proto::Manifest(requestId, taskId, requestCount, responseCount);
    } else {
        // Task is not yet done; pongHeader.branchComplete = false;
        new (&pongHeader.manifest) Proto::Manifest(requestId, taskId, 0, 0);
    }
    socket->transport->getDriver()->addressToWireFormat(
        socket->transport->getDriver()->getLocalAddress(),
        &pongHeader.manifest.serverAddress);

    Homa::unique_ptr<Homa::OutMessage> pong = socket->transport->alloc();
    pong->append(&pongHeader, sizeof(Proto::PongHeader));
    pong->send(replyAddress,
               Homa::OutMessage::NO_RETRY | Homa::OutMessage::NO_KEEP_ALIVE);
}

/**
 * Handle a timeout event.
 *
 * @return
 *      True if the ServerTask timeout should be reset; false if the ServerTask
 *      has expired.
 */
bool
ServerTaskImpl::handleTimeout()
{
    SpinLock::Lock lock(pingInfo.mutex);
    if (pingInfo.pingCount > 0) {
        pingInfo.pingCount = 0;
        return true;
    } else {
        return false;
    }
}

/**
 * @copydoc ServerTask::destroy()
 */
void
ServerTaskImpl::destroy()
{
    Perf::Timer timer;

    if (responseCount + requestCount == 0) {
        // Task didn't generate any outbound messages; send Manifest message.
        Homa::unique_ptr<Homa::OutMessage> message = socket->transport->alloc();
        Proto::ManifestHeader header(rooId, hasUnsentManifest ? 2 : 1);
        Proto::Manifest manifest(requestId, taskId, requestCount,
                                 responseCount);
        socket->transport->getDriver()->addressToWireFormat(
            socket->transport->getDriver()->getLocalAddress(),
            &manifest.serverAddress);
        message->append(&header, sizeof(Proto::ManifestHeader));
        if (hasUnsentManifest) {
            message->append(&delegatedManifest, sizeof(Proto::Manifest));
        }
        message->append(&manifest, sizeof(Proto::Manifest));
        message->send(replyAddress, Homa::OutMessage::NO_RETRY |
                                        Homa::OutMessage::NO_KEEP_ALIVE);
        pendingMessages.push_back(message.get());
        outboundMessages.push_back(std::move(message));
    } else if (responseCount + requestCount == 1) {
        // Only a single outbound message; use Manifest elimination.
        assert(bufferedMessage);
        if (bufferedMessageIsRequest) {
            bufferedRequestHeader.requestId =
                Proto::RequestId(requestId.branchId, requestId.sequence + 1);
        } else {
            bufferedResponseHeader.manifestImplied = true;
        }
        sendBufferedMessage();
    } else {
        // More than 1 outbound message; piggy-back the manifest for this task.
        assert(bufferedMessage);
        assert(!hasUnsentManifest);  // Any previously delegated manifest should
                                     // have already been forwarded.

        Proto::Manifest manifest(requestId, taskId, requestCount,
                                 responseCount);
        socket->transport->getDriver()->addressToWireFormat(
            socket->transport->getDriver()->getLocalAddress(),
            &manifest.serverAddress);
        if (bufferedMessageIsRequest) {
            assert(!bufferedRequestHeader.hasManifest);
            bufferedRequestHeader.hasManifest = true;
            bufferedRequestHeader.manifest = manifest;
        } else {
            assert(!bufferedResponseHeader.hasManifest);
            bufferedResponseHeader.hasManifest = true;
            bufferedResponseHeader.manifest = manifest;
        }
        sendBufferedMessage();
    }

    // Don't delete the ServerTask yet.  Just pass it to the socket so it can
    // make sure that any outgoing messages are competely sent.
    detached.store(true);
    socket->remandTask(this);
    Perf::counters.server_api_cycles.add(timer.split());
}

/**
 * Send the buffered message.
 */
void
ServerTaskImpl::sendBufferedMessage()
{
    if (bufferedMessage) {
        Perf::counters.tx_message_bytes.add(bufferedMessage->length());
        if (bufferedMessageIsRequest) {
            bufferedMessage->prepend(&bufferedRequestHeader,
                                     sizeof(bufferedRequestHeader));
        } else {
            bufferedMessage->prepend(&bufferedResponseHeader,
                                     sizeof(bufferedResponseHeader));
        }
        bufferedMessage->send(
            bufferedMessageAddress,
            Homa::OutMessage::NO_RETRY | Homa::OutMessage::NO_KEEP_ALIVE);
        pendingMessages.push_back(bufferedMessage.get());
        outboundMessages.push_back(std::move(bufferedMessage));
    }
}

}  // namespace Roo
