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

#include "SessionImpl.h"

namespace Roo {

/**
 * RooPCImpl constructor.
 */
RooPCImpl::RooPCImpl(SessionImpl* session, Proto::RooId rooId)
    : session(session)
    , rooId(rooId)
    , pendingRequest(nullptr)
    , response(nullptr)
{}

/**
 * RooPCImpl destructor.
 */
RooPCImpl::~RooPCImpl()
{
    if (pendingRequest != nullptr) {
        pendingRequest->release();
    }
    if (response != nullptr) {
        response->release();
    }
}

/**
 * @copydoc RooPCImpl::allocRequest()
 */
Homa::OutMessage*
RooPCImpl::allocRequest()
{
    Homa::OutMessage* message = session->transport->alloc();
    message->reserve(sizeof(Proto::Message::Header));
    return message;
}

/**
 * @copydoc RooPCImpl::send()
 */
void
RooPCImpl::send(Homa::Driver::Address destination, Homa::OutMessage* request)
{
    Homa::Driver::Address replyAddress =
        session->transport->getDriver()->getLocalAddress();
    Proto::RequestId requestId = session->allocRequestId();
    Proto::Message::Header outboundHeader(rooId, requestId,
                                          Proto::Message::Type::Initial);
    session->transport->getDriver()->addressToWireFormat(
        replyAddress, &outboundHeader.replyAddress);
    request->prepend(&outboundHeader, sizeof(outboundHeader));
    pendingRequest = request;
    request->send(destination);
}

/**
 * @copydoc RooPCImpl::receive()
 */
Homa::InMessage*
RooPCImpl::receive()
{
    return response;
}

/**
 * @copydoc RooPCImpl::checkStatus()
 */
RooPC::Status
RooPCImpl::checkStatus()
{
    if (pendingRequest == nullptr) {
        return Status::NOT_STARTED;
    } else if (pendingRequest->getStatus() ==
               Homa::OutMessage::Status::FAILED) {
        return Status::FAILED;
    } else if (response == nullptr) {
        return Status::IN_PROGRESS;
    } else {
        return Status::COMPLETED;
    }
}

/**
 * @copydoc RooPCImpl::wait()
 */
void
RooPCImpl::wait()
{
    while (checkStatus() == Status::IN_PROGRESS) {
        session->poll();
    }
}

/**
 * @copydoc RooPCImpl::destroy()
 */
void
RooPCImpl::destroy()
{
    // Don't actually free the object yet.  Return contol to the managing
    // session so it can do some clean up.
    session->dropRooPC(this);
}

/**
 * Add the incoming response message to this RooPC.
 *
 * @param message
 *      The incoming response message to add.
 */
void
RooPCImpl::queueResponse(Homa::InMessage* message)
{
    response = message;
    pendingRequest->cancel();
}

}  // namespace Roo
