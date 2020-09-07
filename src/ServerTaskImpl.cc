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

#include <vector>

#include "ControlMessage.h"
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
    , pingInfo()
    , bufferedMessageIsRequest(false)
    , bufferedMessageAddress()
    , bufferedMessageHeader()
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
    new (bufferedResponseHeader) Proto::ResponseHeader(
        rooId, requestId.branchId, Proto::ResponseId(taskId, responseCount));
    responseCount += 1;
    if (hasUnsentManifest) {
        // piggy-back the delegated manifest
        bufferedResponseHeader->hasManifest = true;
        bufferedResponseHeader->manifest = delegatedManifest;
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
    new (bufferedRequestHeader) Proto::RequestHeader(rooId, newRequestId);
    socket->transport->getDriver()->addressToWireFormat(
        replyAddress, &bufferedRequestHeader->replyAddress);
    if (hasUnsentManifest) {
        // piggy-back the delegated manifest
        bufferedRequestHeader->hasManifest = true;
        bufferedRequestHeader->manifest = delegatedManifest;
        hasUnsentManifest = false;
    }
    bufferedMessageAddress = destination;
    bufferedMessage = std::move(message);

    Perf::counters.server_api_cycles.add(timer.split());
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

    // Send back a Pong with all availble current information about this task.

    // The Pong should only include sent requests and not any request which
    // is currently being buffered. This is because a client will expect to
    // be able to ping the delegated request; if the request sending is delayed,
    // the client pings will not succeed.

    // Branch is complete if detached and not in the special case where the task
    // has only a single delegate request.
    const bool isTaskComplete = detached;
    const bool isBranchComplete =
        isTaskComplete && !(requestCount == 1 && responseCount == 0);
    const uint32_t requestsSent = pingInfo.destinations.size();
    std::vector<Homa::Driver::WireFormatAddress> destinations;
    for (auto& destination : pingInfo.destinations) {
        destinations.push_back({});
        socket->transport->getDriver()->addressToWireFormat(
            destination, &destinations.back());
    }

    ControlMessage::sendWithPayload<Proto::PongHeader>(
        socket->transport, replyAddress, destinations.data(),
        sizeof(Homa::Driver::WireFormatAddress) * requestsSent, rooId,
        requestId, taskId, requestsSent, responseCount, isTaskComplete,
        isBranchComplete);
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
    if (!detached.load()) {
        return true;
    } else if (pingInfo.pingCount > 0) {
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
    } else if (responseCount + requestCount == 1) {
        // Only a single outbound message; use Manifest elimination.
        assert(bufferedMessage);
        if (bufferedMessageIsRequest) {
            bufferedRequestHeader->requestId =
                Proto::RequestId(requestId.branchId, requestId.sequence + 1);
        } else {
            bufferedResponseHeader->manifestImplied = true;
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
            assert(!bufferedRequestHeader->hasManifest);
            bufferedRequestHeader->hasManifest = true;
            bufferedRequestHeader->manifest = manifest;
        } else {
            assert(!bufferedResponseHeader->hasManifest);
            bufferedResponseHeader->hasManifest = true;
            bufferedResponseHeader->manifest = manifest;
        }
        sendBufferedMessage();
    }

    // Request mesage no longer needed.
    request.reset();

    // Don't delete the ServerTask yet; mark it as detached so that the Socket
    // will know when it asks this ServerTask to handle the task timeout.
    detached.store(true);
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
            bufferedMessage->prepend(bufferedRequestHeader,
                                     sizeof(Proto::RequestHeader));
            SpinLock::Lock lock(pingInfo.mutex);
            // Assert that the destinations are added in order.
            assert(
                (Proto::RequestId{
                     {taskId, (uint32_t)pingInfo.destinations.size()}, 0} ==
                 bufferedRequestHeader->requestId) ||
                (Proto::RequestId{requestId.branchId, requestId.sequence + 1} ==
                     bufferedRequestHeader->requestId &&
                 pingInfo.destinations.empty()));
            pingInfo.destinations.push_back(bufferedMessageAddress);
        } else {
            bufferedMessage->prepend(bufferedResponseHeader,
                                     sizeof(Proto::ResponseHeader));
        }
        bufferedMessage->send(
            bufferedMessageAddress,
            Homa::OutMessage::NO_RETRY | Homa::OutMessage::NO_KEEP_ALIVE);
        bufferedMessage.reset();
    }
}

}  // namespace Roo
