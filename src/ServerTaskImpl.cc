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
#include "SocketImpl.h"

namespace Roo {

/**
 * ServerTaskImpl constructor.
 *
 * @param socket
 * @param rooId
 */
ServerTaskImpl::ServerTaskImpl(SocketImpl* socket,
                               Proto::Message::Header const* requestHeader,
                               Homa::unique_ptr<Homa::InMessage> request)
    : state(State::IN_PROGRESS)
    , detached(false)
    , socket(socket)
    , rooId(requestHeader->rooId)
    , requestId(requestHeader->requestId)
    , isInitialRequest(requestHeader->type == Proto::Message::Type::Initial)
    , request(std::move(request))
    , replyAddress(socket->transport->getDriver()->getAddress(
          &requestHeader->replyAddress))
    , response()
    , pendingRequests()
    , delegationUpdate()
{
    this->request->strip(sizeof(Proto::Message::Header));
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
    message->reserve(sizeof(Proto::Message::Header));
    return message;
}

/**
 * @copydoc ServerTask::reply()
 */
void
ServerTaskImpl::reply(Homa::unique_ptr<Homa::OutMessage> message)
{
    assert(!response);
    assert(pendingRequests.empty());
    Proto::Message::Header header(rooId, requestId,
                                  Proto::Message::Type::Response);
    socket->transport->getDriver()->addressToWireFormat(replyAddress,
                                                        &header.replyAddress);
    response = std::move(message);
    response->prepend(&header, sizeof(header));
    response->send(replyAddress);
}

/**
 * @copydoc ServerTask::delegate()
 */
void
ServerTaskImpl::delegate(Homa::Driver::Address destination,
                         Homa::unique_ptr<Homa::OutMessage> message)
{
    assert(!response);
    if (!delegationUpdate) {
        delegationUpdate = std::move(socket->transport->alloc());
        delegationUpdate->reserve(sizeof(Proto::Delegation::Header));
    }
    Proto::RequestId newRequestId = socket->allocRequestId();
    Proto::Message::Header header(rooId, newRequestId,
                                  Proto::Message::Type::Request);
    socket->transport->getDriver()->addressToWireFormat(replyAddress,
                                                        &header.replyAddress);
    message->prepend(&header, sizeof(header));
    message->send(destination);
    pendingRequests.push_back(std::move(message));
    delegationUpdate->append(&newRequestId, sizeof(Proto::RequestId));
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
    } else if (response != nullptr) {
        // ServerTask sent a reply
        if (response->getStatus() == Homa::OutMessage::Status::COMPLETED) {
            // Done
            if (!isInitialRequest) {
                request->acknowledge();
            }
            return false;
        } else {
            return true;
        }
    } else if (!pendingRequests.empty()) {
        // ServerTask delegated
        for (auto it = pendingRequests.begin(); it != pendingRequests.end();
             ++it) {
            Homa::OutMessage* pendingRequest = it->get();
            Homa::OutMessage::Status status = pendingRequest->getStatus();
            if (status == Homa::OutMessage::Status::COMPLETED) {
                // Keep checking for other pendingRequests
            } else if (status == Homa::OutMessage::Status::FAILED) {
                request->fail();
                // Failed, no need to keep checking
                return false;
            } else {
                // Unfinished requests; keep polling
                return true;
            }
        }
        assert(delegationUpdate);
        Homa::OutMessage::Status status = delegationUpdate->getStatus();
        if (status == Homa::OutMessage::Status::FAILED) {
            request->fail();
            return false;
        } else if (status != Homa::OutMessage::Status::COMPLETED) {
            // Keep checking
            return true;
        }
        // All pending requests the delegationUpdate is complete.
        if (!isInitialRequest) {
            request->acknowledge();
        }
        return false;
    } else {
        // Nothing left to do
        return false;
    }
    // Can't reach this state
}

/**
 * @copydoc ServerTask::destroy()
 */
void
ServerTaskImpl::destroy()
{
    // Send out the delegation information
    if (!pendingRequests.empty()) {
        Proto::Delegation::Header delegationHeader(rooId, requestId,
                                                   pendingRequests.size());
        assert(delegationUpdate != nullptr);
        delegationUpdate->prepend(&delegationHeader,
                                  sizeof(Proto::Delegation::Header));
        delegationUpdate->send(replyAddress);
    }

    // Don't delete the ServerTask yet.  Just pass it to the socket so it can
    // make sure that any outgoing messages are competely sent.
    detached.store(true);
    socket->remandTask(this);
}

}  // namespace Roo
