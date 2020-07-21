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

#ifndef ROO_SERVERTASKIMPL_H
#define ROO_SERVERTASKIMPL_H

#include <Homa/Homa.h>
#include <Roo/Roo.h>

#include <deque>

#include "Proto.h"

namespace Roo {

// Forward declaration
class SocketImpl;

/**
 * Implementation of Roo::ServerTask.
 *
 * This class is NOT thread-safe.
 */
class ServerTaskImpl : public ServerTask {
  public:
    explicit ServerTaskImpl(SocketImpl* socket, Proto::TaskId taskId,
                            Proto::RequestHeader const* requestHeader,
                            Homa::unique_ptr<Homa::InMessage> request);
    virtual ~ServerTaskImpl();
    virtual Homa::InMessage* getRequest();
    virtual void reply(const void* response, std::size_t length);
    virtual void delegate(Homa::Driver::Address destination,
                          const void* request, std::size_t length);
    bool poll();

  protected:
    virtual void destroy();

  private:
    void sendBufferedMessage();

    /// True if the ServerTask is no longer held by the application and is being
    /// processed by the Socket.
    std::atomic<bool> detached;

    // The socket that manages this ServerTask.
    SocketImpl* const socket;

    /// Identifier the RooPC that triggered this ServerTask.
    Proto::RooId const rooId;

    /// Identifier for the request branch to which this ServerTask belongs.
    Proto::BranchId const branchId;

    /// Identifier for this task.
    Proto::TaskId const taskId;

    /// Identify whether the request can directly from a RooPC client.
    bool const isInitialRequest;

    /// Message containing a task request; may come directly from the RooPC
    /// client, or from another server that has delegated a request to us.
    Homa::unique_ptr<Homa::InMessage> const request;

    /// Address of the client that sent the original request; the reply should
    /// be sent back to this address.
    Homa::Driver::Address const replyAddress;

    /// Number of responses sent by this task.
    uint32_t responseCount;

    /// Number of delegated requests sent by this task.
    uint32_t requestCount;

    /// Messages (include responses, delegated requests, and manifest messages),
    //. that have been sent by this task.
    std::deque<Homa::unique_ptr<Homa::OutMessage>> outboundMessages;

    /// Messages that have been sent by this task but have not yet completed.
    std::deque<Homa::OutMessage*> pendingMessages;

    /// True if the buffered message is a request. False if the buffered message
    /// is a response.
    bool bufferedMessageIsRequest;

    /// Address to which the buffered message should be sent.
    Homa::Driver::Address bufferedMessageAddress;

    /// Header for the buffered message if the message is a request.
    Proto::RequestHeader bufferedRequestHeader;

    /// Header for the buffered message if the message is a response.
    Proto::ResponseHeader bufferedResponseHeader;

    /// A request or response message that has been buffered to be sent later.
    Homa::unique_ptr<Homa::OutMessage> bufferedMessage;

    /// True if a manifest that was piggy-backed on the incoming request still
    /// needs to be sent.
    bool hasUnsentManifest;

    /// Holds a manifest that was piggy-backed on the incoming request.
    Proto::Manifest delegatedManifest;
};

}  // namespace Roo

#endif  // ROO_SERVERTASKIMPL_H
