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
#include "SessionImpl.h"

namespace Roo {

/**
 * ServerTaskImpl constructor.
 *
 * @param session
 * @param rooId
 */
ServerTaskImpl::ServerTaskImpl(SessionImpl* session,
                               Proto::Message::Header const* requestHeader,
                               Homa::InMessage* request)
    : state(State::IN_PROGRESS)
    , detached(false)
    , session(session)
    , rooId(requestHeader->rooId)
    , stageId(requestHeader->stageId)
    , request(request)
    , replyAddress(session->transport->getDriver()->getAddress(
          &requestHeader->replyAddress))
    , response(nullptr)
    , pendingRequest(nullptr)
{
    request->release();
    if (response != nullptr) {
        response->release();
    }
    if (pendingRequest != nullptr) {
        pendingRequest->release();
    }
}

/**
 * ServerTaskImpl destructor.
 */
ServerTaskImpl::~ServerTaskImpl() {}

/**
 * @copydoc ServerTask::getRequest()
 */
Homa::InMessage*
ServerTaskImpl::getRequest()
{
    return request;
}

/**
 * @copydoc ServerTask::allocOutMessage()
 */
Homa::OutMessage*
ServerTaskImpl::allocOutMessage()
{
    Homa::OutMessage* message = session->transport->alloc();
    message->reserve(sizeof(Proto::Message::Header));
    return message;
}

/**
 * @copydoc ServerTask::reply()
 */
void
ServerTaskImpl::reply(Homa::OutMessage* message)
{
    Proto::Message::Header header(rooId, Proto::Message::ULTIMATE_RESPONSE_ID);
    session->transport->getDriver()->addressToWireFormat(replyAddress,
                                                         &header.replyAddress);
    response = message;
    response->prepend(&header, sizeof(header));
    response->send(replyAddress);
}

/**
 * @copydoc ServerTask::delegate()
 */
void
ServerTaskImpl::delegate(Homa::Driver::Address destination,
                         Homa::OutMessage* message)
{
    Proto::Message::Header header(rooId, stageId + 1);
    session->transport->getDriver()->addressToWireFormat(replyAddress,
                                                         &header.replyAddress);
    pendingRequest = message;
    pendingRequest->prepend(&header, sizeof(header));
    pendingRequest->send(destination);
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
    bool polling = true;
    State copyOfState = state.load();
    Homa::OutMessage::Status outState = Homa::OutMessage::Status::NOT_STARTED;
    if (copyOfState == State::IN_PROGRESS) {
        if (request->dropped()) {
            state.store(State::DROPPED);
        } else if (response != nullptr) {
            outState = response->getStatus();
            if (outState == Homa::OutMessage::Status::SENT) {
                state.store(State::COMPLETED);
                if (stageId != Proto::Message::INITIAL_REQUEST_ID) {
                    request->acknowledge();
                }
            }
        } else if (pendingRequest != nullptr) {
            outState = pendingRequest->getStatus();
            if (outState == Homa::OutMessage::Status::COMPLETED) {
                state.store(State::COMPLETED);
                if (stageId != Proto::Message::INITIAL_REQUEST_ID) {
                    request->acknowledge();
                }
            } else if (outState == Homa::OutMessage::Status::FAILED) {
                state.store(State::FAILED);
                // Deregister the outbound message in case the application wants
                // to try again.
                pendingRequest->cancel();
            }
        }
    } else if (copyOfState == State::COMPLETED) {
        // Nothing to do.
        polling = false;
    } else if (copyOfState == State::DROPPED) {
        // Nothing to do.
        polling = false;
    } else if (copyOfState == State::FAILED) {
        if (detached) {
            assert(request != nullptr);
            // If detached, automatically return an ERROR back to the Sender now
            // that the Server has given up.
            request->fail();
        }
        polling = false;
    } else {
        PANIC("Unknown ServerTask state.");
    }
    return polling;
}

/**
 * @copydoc ServerTask::destroy()
 */
void
ServerTaskImpl::destroy()
{
    // Don't delete the ServerTask yet.  Just pass it to the session so it can
    // make sure that any outgoing messages are competely sent.
    detached.store(true);
    session->remandTask(this);
}

}  // namespace Roo
