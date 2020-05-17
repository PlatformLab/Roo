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

    void handleResponse(Proto::ResponseHeader* header,
                        Homa::unique_ptr<Homa::InMessage> message);
    void handleManifest(Proto::Manifest* manifest,
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
    void markManifestReceived(Proto::BranchId branchId);

    /// Monitor-style lock
    SpinLock mutex;

    /// Socket that manages this RooPC.
    SocketImpl* const socket;

    /// Unique identifier for this RooPC.
    Proto::RooId rooId;

    /// Number of requests sent.
    uint64_t requestCount;

    /// Requests that have been sent for this RooPC.
    std::deque<Homa::unique_ptr<Homa::OutMessage> > pendingRequests;

    /// Responses for this RooPC that have not yet been delievered.
    std::deque<Homa::unique_ptr<Homa::InMessage> > responseQueue;

    /// Tracks the tasks spawned from RooPC. Maps from the identifer of the
    /// request branch that spawned the task to a boolean value. The value is
    /// false, if a manifest for the task has not yet been received; otherwise,
    /// the value is true.
    std::unordered_map<Proto::BranchId, bool, Proto::BranchId::Hasher> tasks;

    /// The number of expected branch manifests that have not yet been
    /// received. (Tracked seperately so the _tasks_ structure doesn't need to
    /// be scanned).
    int manifestsOutstanding;

    /// Tracks whether or not expected responses have been received.  Maps from
    /// a response identifer to a boolean value.  The value is true, if the
    /// response has been received and false if the response is expected but
    /// has not yet been received.
    std::unordered_map<Proto::ResponseId, bool, Proto::ResponseId::Hasher>
        expectedResponses;

    /// The number of expected responses that have not yet been received.
    int responsesOutstanding;
};

}  // namespace Roo

#endif  // ROO_ROOPCIMPL_H
