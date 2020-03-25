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

#ifndef ROO_ROOPCIMPL_H
#define ROO_ROOPCIMPL_H

#include <Roo/Roo.h>

#include <deque>
#include <unordered_map>

#include "Proto.h"
#include "SpinLock.h"

namespace Roo {

// Forward Declaration
class SocketImpl;

/**
 * Implementation of RooPC.
 */
class RooPCImpl : public RooPC {
  public:
    explicit RooPCImpl(SocketImpl* socket, Proto::RooId rooId);
    virtual ~RooPCImpl();
    virtual Homa::unique_ptr<Homa::OutMessage> allocRequest();
    virtual void send(Homa::Driver::Address destination,
                      Homa::unique_ptr<Homa::OutMessage> request);
    virtual Homa::unique_ptr<Homa::InMessage> receive();
    virtual Status checkStatus();
    virtual void wait();

    void handleResponse(Proto::Message::Header* header,
                        Homa::unique_ptr<Homa::InMessage> message);
    void handleDelegation(Proto::Delegation::Header* header,
                          Homa::unique_ptr<Homa::InMessage> message);

    /**
     * Return this RooPC's identifier.
     */
    Proto::RooId getId()
    {
        return rooId;
    }

  protected:
    virtual void destroy();

  private:
    /// Monitor-style lock
    SpinLock mutex;

    /// Socket that manages this RooPC.
    SocketImpl* const socket;

    /// Unique identifier for this RooPC.
    Proto::RooId rooId;

    /// Requests that have been sent for this RooPC.
    std::deque<Homa::unique_ptr<Homa::OutMessage> > pendingRequests;

    /// Responses for this RooPC that have not yet been delievered.
    std::deque<Homa::unique_ptr<Homa::InMessage> > responseQueue;

    /// The number of expected responses that have not yet been received.
    int responsesOutstanding;

    /**
     * Tracking status for incoming responses.
     */
    enum class ResponseStatus {
        Expected,    ///< Response is expected but not yet received.
        Unexpected,  ///< Response has been received but the delegation
                     ///< confirmation for this response has not yet arrived.
        Complete,    ///< Both the response and the delegation confirmation has
                     ///< been received.
    };

    /// Keeps track of the response this RooPC expects to receive.
    std::unordered_map<Proto::RequestId, ResponseStatus,
                       Proto::RequestId::Hasher>
        expectedResponses;
};

}  // namespace Roo

#endif  // ROO_ROOPCIMPL_H
