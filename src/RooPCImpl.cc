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
#include "SocketImpl.h"

namespace Roo {

/**
 * RooPCImpl constructor.
 */
RooPCImpl::RooPCImpl(SocketImpl* socket, Proto::RooId rooId)
    : socket(socket)
    , rooId(rooId)
    , pendingRequests()
    , responseQueue()
    , responsesOutstanding(0)
    , expectedResponses()
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
    message->reserve(sizeof(Proto::Message::Header));
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
    Homa::Driver::Address replyAddress =
        socket->transport->getDriver()->getLocalAddress();
    Proto::RequestId requestId = socket->allocRequestId();
    Proto::Message::Header outboundHeader(rooId, requestId,
                                          Proto::Message::Type::Initial);
    socket->transport->getDriver()->addressToWireFormat(
        replyAddress, &outboundHeader.replyAddress);
    request->prepend(&outboundHeader, sizeof(outboundHeader));
    expectedResponses.insert({requestId, ResponseStatus::Expected});
    responsesOutstanding++;
    request->send(destination);
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
    if (pendingRequests.empty()) {
        return Status::NOT_STARTED;
    } else if (responsesOutstanding == 0) {
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
RooPCImpl::handleResponse(Proto::Message::Header* header,
                          Homa::unique_ptr<Homa::InMessage> message)
{
    SpinLock::Lock lock(mutex);
    message->strip(sizeof(Proto::Message::Header));
    auto ret = expectedResponses.insert(
        {header->requestId, ResponseStatus::Unexpected});
    if (ret.second) {
        // New unanticipated response received.
        responsesOutstanding++;
        responseQueue.push_back(std::move(message));
    } else if (ret.first->second == ResponseStatus::Expected) {
        // Expected response received.
        ret.first->second = ResponseStatus::Complete;
        responsesOutstanding--;
        responseQueue.push_back(std::move(message));
    } else {
        // Response already received
        NOTICE("Duplicate response received for RooPC (%lu, %lu)",
               rooId.socketId, rooId.sequence);
    }
}

void
RooPCImpl::handleDelegation(Proto::Delegation::Header* header,
                            Homa::unique_ptr<Homa::InMessage> message)
{
    SpinLock::Lock lock(mutex);
    auto ret = expectedResponses.insert(
        {header->requestId, ResponseStatus::Unexpected});
    if (ret.second) {
        // New delegation received.
        responsesOutstanding++;
    } else if (ret.first->second == ResponseStatus::Expected) {
        ret.first->second = ResponseStatus::Complete;
        responsesOutstanding--;
    } else {
        WARNING(
            "Duplicate delegation confirmation received for RooPC (%lu, %lu)",
            rooId.socketId, rooId.sequence);
        return;
    }

    // Add new expected requests
    message->strip(sizeof(Proto::Delegation::Header));
    for (uint64_t i = 0; i < header->num; ++i) {
        Proto::RequestId requestId;
        message->get(sizeof(requestId) * i, &requestId, sizeof(requestId));
        auto ret =
            expectedResponses.insert({requestId, ResponseStatus::Expected});
        if (ret.second) {
            // Response not yet received.
            responsesOutstanding++;
        } else if (ret.first->second == ResponseStatus::Unexpected) {
            // Response already arrived.
            ret.first->second = ResponseStatus::Complete;
            responsesOutstanding--;
        } else {
            WARNING(
                "Duplicate delegation confirmation received for RooPC (%lu, "
                "%lu)",
                rooId.socketId, rooId.sequence);
        }
    }
    message->acknowledge();
}

}  // namespace Roo
