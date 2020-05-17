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
    , isInitialRequest(branchId.taskId == rooId)
    , request(std::move(request))
    , replyAddress(socket->transport->getDriver()->getAddress(
          &requestHeader->replyAddress))
    , responseCount(0)
    , requestCount(0)
    , outboundMessages()
    , pendingMessages()
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
    Perf::counters.tx_message_bytes.add(message->length());
    Proto::ResponseHeader header(rooId, branchId,
                                 Proto::ResponseId(taskId, responseCount));
    responseCount += 1;
    message->prepend(&header, sizeof(header));
    message->send(replyAddress);
    pendingMessages.push_back(message.get());
    outboundMessages.push_back(std::move(message));
}

/**
 * @copydoc ServerTask::delegate()
 */
void
ServerTaskImpl::delegate(Homa::Driver::Address destination,
                         Homa::unique_ptr<Homa::OutMessage> message)
{
    Perf::counters.tx_message_bytes.add(message->length());
    Proto::BranchId newBranchId(taskId, requestCount);
    requestCount += 1;
    Proto::RequestHeader header(rooId, newBranchId);
    socket->transport->getDriver()->addressToWireFormat(replyAddress,
                                                        &header.replyAddress);
    message->prepend(&header, sizeof(header));
    message->send(destination);
    pendingMessages.push_back(message.get());
    outboundMessages.push_back(std::move(message));
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
    if (request->dropped()) {
        // Nothing left to do
        return false;
    } else if (pendingMessages.empty()) {
        // No more pending messages.
        if (!isInitialRequest) {
            request->acknowledge();
        }
        return false;
    } else {
        // Check for any remaining pending messages
        auto it = pendingMessages.begin();
        while (it != pendingMessages.end()) {
            Homa::OutMessage::Status status = (*it)->getStatus();
            if (status == Homa::OutMessage::Status::COMPLETED) {
                // Remove and keep checking for other pendingRequests
                it = pendingMessages.erase(it);
            } else if (status == Homa::OutMessage::Status::FAILED) {
                request->fail();
                // Failed, no need to keep checking
                return false;
            } else {
                ++it;
            }
        }
    }
    return true;
}

/**
 * @copydoc ServerTask::destroy()
 */
void
ServerTaskImpl::destroy()
{
    // Send out the manifest information
    Homa::unique_ptr<Homa::OutMessage> message = socket->transport->alloc();
    Proto::Manifest manifest(rooId, branchId, taskId, requestCount,
                             responseCount);
    message->append(&manifest, sizeof(Proto::Manifest));
    message->send(replyAddress);
    pendingMessages.push_back(message.get());
    outboundMessages.push_back(std::move(message));

    // Don't delete the ServerTask yet.  Just pass it to the socket so it can
    // make sure that any outgoing messages are competely sent.
    detached.store(true);
    socket->remandTask(this);
}

}  // namespace Roo
