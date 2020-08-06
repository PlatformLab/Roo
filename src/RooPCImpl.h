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
    virtual void send(Homa::Driver::Address destination, const void* request,
                      std::size_t length);
    virtual Homa::InMessage* receive();
    virtual Status checkStatus();
    virtual void wait();

    void handleResponse(Proto::ResponseHeader* header,
                        Homa::unique_ptr<Homa::InMessage> message);
    void handleManifest(Proto::ManifestHeader* header,
                        Homa::unique_ptr<Homa::InMessage> message);
    void handlePong(Proto::PongHeader* header,
                    Homa::unique_ptr<Homa::InMessage> message);
    void handleError(Proto::ErrorHeader* header,
                     Homa::unique_ptr<Homa::InMessage> message);
    bool handleTimeout();

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
    /**
     * Metadata for a task spawned from this RooPC.
     */
    struct TaskInfo {
        /// True, if a manifest for this task has been received signaling that
        /// the task has finished processing; false, otherwise.
        bool complete;

        /// Id of the request that should be pinged to keep this task alive.
        Proto::RequestId pingRequestId;

        /// Address to which pings should be sent.
        Homa::Driver::Address pingAddress;

        /// The number of pings sent since last received pong response.
        uint pingCount;
    };

    void markManifestReceived(Proto::BranchId branchId,
                              const SpinLock::Lock& lock);
    void processManifest(Proto::Manifest* manifest, const SpinLock::Lock& lock);

    /// Monitor-style lock
    SpinLock mutex;

    /// Socket that manages this RooPC.
    SocketImpl* const socket;

    /// Unique identifier for this RooPC.
    Proto::RooId rooId;

    /// True if an error message was received; false, otherwise.
    bool error;

    /// Number of requests sent.
    uint64_t requestCount;

    /// Requests that have been sent for this RooPC.
    std::deque<Homa::unique_ptr<Homa::OutMessage> > pendingRequests;

    /// Responses for this RooPC that have not yet been delievered.
    std::deque<Homa::InMessage*> responseQueue;

    /// All responses that have been received.
    std::deque<Homa::unique_ptr<Homa::InMessage> > responses;

    /// Ping messages sent during the last timeout.
    std::deque<Homa::unique_ptr<Homa::OutMessage> > pings;

    /// Tracks the tasks spawned from RooPC.
    std::unordered_map<Proto::BranchId, TaskInfo, Proto::BranchId::Hasher>
        tasks;

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
