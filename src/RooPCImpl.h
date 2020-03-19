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

#include "Proto.h"

namespace Roo {

// Forward Declaration
class SessionImpl;

/**
 * Implementation of RooPC.
 */
class RooPCImpl : public RooPC {
  public:
    explicit RooPCImpl(SessionImpl* session, Proto::RooId rooId);
    virtual ~RooPCImpl();
    virtual Homa::OutMessage* allocRequest();
    virtual void send(Homa::Driver::Address destination,
                      Homa::OutMessage* request);
    virtual Homa::InMessage* receive();
    virtual Status checkStatus();
    virtual void wait();

    void queueResponse(Homa::InMessage* message);

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
    /// Session that manages this RooPC.
    SessionImpl* const session;

    /// Unique identifier for this RooPC.
    Proto::RooId rooId;

    /// Request that has been sent for this RooPC.
    Homa::OutMessage* pendingRequest;

    /// Message containing the result of processing the RooPC request.
    Homa::InMessage* response;
};

}  // namespace Roo

#endif  // ROO_ROOPCIMPL_H
