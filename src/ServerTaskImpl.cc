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
    , branchId(requestHeader->branchId)
    , taskId(taskId)
    , isInitialRequest(requestHeader->isFromClient)
    , request(std::move(request))
    , replyAddress(socket->transport->getDriver()->getAddress(
          &requestHeader->replyAddress))
    , responseCount(0)
    , requestCount(0)
    , outboundMessages()
    , pendingMessages()
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
 * @copydoc ServerTask::allocOutMessage()
 */
Homa::unique_ptr<Homa::OutMessage>
ServerTaskImpl::allocOutMessage()
{
    Homa::unique_ptr<Homa::OutMessage> message = socket->transport->alloc();
    // TODO(cstlee): Change API or Homa so that the request and response headers
    //               don't have to be the same size.
    // Make sure the request and response headers are the same size so that we
    // can allocate the same amount of space for the header no matter if this
    // OutMessage will be used for a request or a response.
    static_assert(sizeof(Proto::RequestHeader) <=
                  sizeof(Proto::ResponseHeader));
    static_assert(sizeof(Proto::RequestHeader) ==
                  sizeof(Proto::ResponseHeader));
    message->reserve(sizeof(Proto::RequestHeader));
    return message;
}

/**
 * @copydoc ServerTask::reply()
 */
void
ServerTaskImpl::reply(Homa::unique_ptr<Homa::OutMessage> message)
{
    // The ServerTask always buffers that last outbound message so that any
    // necessary manifest information can be piggy-back on the last message.
    // The response message provided in this call will be sent when the server
    // next calls either reply() or delegate() or when server is done processing
    // the ServerTask.

    // Send out any previously buffered message
    sendBufferedMessage();

    // Format the response message
    bufferedMessageIsRequest = false;
    new (&bufferedResponseHeader) Proto::ResponseHeader(
        rooId, branchId, Proto::ResponseId(taskId, responseCount));
    responseCount += 1;
    if (hasUnsentManifest) {
        // piggy-back the delegated manifest
        bufferedResponseHeader.hasManifest = true;
        bufferedResponseHeader.manifest = delegatedManifest;
        hasUnsentManifest = false;
    }
    bufferedMessageAddress = replyAddress;
    bufferedMessage = std::move(message);
}

/**
 * @copydoc ServerTask::delegate()
 */
void
ServerTaskImpl::delegate(Homa::Driver::Address destination,
                         Homa::unique_ptr<Homa::OutMessage> message)
{
    // The ServerTask always buffers that last outbound message so that any
    // necessary manifest information can be piggy-back on the last message.
    // The request message provided in this call will be sent when the server
    // next calls either reply() or delegate() or when server is done processing
    // the ServerTask.

    // Send out any previously buffered message
    sendBufferedMessage();

    // Format the delegated request message
    bufferedMessageIsRequest = true;
    Proto::BranchId newBranchId(taskId, requestCount);
    requestCount += 1;
    new (&bufferedRequestHeader) Proto::RequestHeader(rooId, newBranchId);
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
    timer.split();
    uint64_t activeTime = 0;
    uint64_t idleTime = 0;

    bool isInProgress = true;

    if (request->dropped()) {
        // Nothing left to do
        isInProgress = false;
        activeTime += timer.split();
    } else if (pendingMessages.empty()) {
        // No more pending messages.
        if (!isInitialRequest) {
            request->acknowledge();
        }
        isInProgress = false;
        activeTime += timer.split();
    } else {
        // Check for any remaining pending messages
        auto it = pendingMessages.begin();
        while (it != pendingMessages.end()) {
            Homa::OutMessage::Status status = (*it)->getStatus();
            if (status == Homa::OutMessage::Status::COMPLETED) {
                // Remove and keep checking for other pendingRequests
                it = pendingMessages.erase(it);
                activeTime += timer.split();
            } else if (status == Homa::OutMessage::Status::FAILED) {
                request->fail();
                // Failed, no need to keep checking
                isInProgress = false;
                activeTime += timer.split();
                break;
            } else {
                ++it;
                idleTime += timer.split();
            }
        }
        idleTime += timer.split();
    }

    Perf::counters.active_cycles.add(activeTime);
    Perf::counters.idle_cycles.add(idleTime);

    return isInProgress;
}

/**
 * @copydoc ServerTask::destroy()
 */
void
ServerTaskImpl::destroy()
{
    if (responseCount + requestCount == 0) {
        // Task didn't generate any outbound messages; send Manifest message.
        Homa::unique_ptr<Homa::OutMessage> message = socket->transport->alloc();
        Proto::ManifestHeader header(rooId, hasUnsentManifest ? 2 : 1);
        Proto::Manifest manifest(branchId, taskId, requestCount, responseCount);
        message->append(&header, sizeof(Proto::ManifestHeader));
        if (hasUnsentManifest) {
            message->append(&delegatedManifest, sizeof(Proto::Manifest));
        }
        message->append(&manifest, sizeof(Proto::Manifest));
        message->send(replyAddress, Homa::OutMessage::NO_RETRY);
        pendingMessages.push_back(message.get());
        outboundMessages.push_back(std::move(message));
    } else if (responseCount + requestCount == 1) {
        // Only a single outbound message; use Manifest elimination.
        assert(bufferedMessage);
        if (bufferedMessageIsRequest) {
            bufferedRequestHeader.branchId = branchId;
            assert(bufferedRequestHeader.branchId == branchId);
        } else {
            bufferedResponseHeader.manifestImplied = true;
        }
        sendBufferedMessage();
    } else {
        // More than 1 outbound message; piggy-back the manifest for this task.
        assert(bufferedMessage);
        assert(!hasUnsentManifest);  // Any previously delegated manifest should
                                     // have already been forwarded.

        Proto::Manifest manifest(branchId, taskId, requestCount, responseCount);
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
        bufferedMessage->send(bufferedMessageAddress,
                              Homa::OutMessage::NO_RETRY);
        pendingMessages.push_back(bufferedMessage.get());
        outboundMessages.push_back(std::move(bufferedMessage));
    }
}

}  // namespace Roo
