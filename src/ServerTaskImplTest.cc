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

#include <gtest/gtest.h>

#include <cstring>

#include "Mock/MockHoma.h"
#include "ServerTaskImpl.h"
#include "SocketImpl.h"

namespace Roo {
namespace {

using ::testing::_;
using ::testing::An;
using ::testing::ByMove;
using ::testing::Eq;
using ::testing::InSequence;
using ::testing::Return;
using ::testing::TypedEq;

class ServerTaskImplTest : public ::testing::Test {
  public:
    ServerTaskImplTest()
        : transport()
        , driver()
        , inMessage()
        , outMessage()
        , replyAddress(0xDEADBEEF)
        , socket(nullptr)
        , handle(nullptr)
        , task(nullptr)
    {
        ON_CALL(transport, getId()).WillByDefault(Return(42));
        ON_CALL(transport, getDriver()).WillByDefault(Return(&driver));

        // Setup default socket
        EXPECT_CALL(transport, getId());
        socket = new SocketImpl(&transport);
    }

    ~ServerTaskImplTest()
    {
        if (handle != nullptr) {
            EXPECT_CALL(inMessage, release());
            socket->taskPool.destroy(handle);
        }
        delete socket;
    }

    void initDefaultTask()
    {
        EXPECT_CALL(transport, getDriver());
        EXPECT_CALL(driver,
                    getAddress(An<const Homa::Driver::WireFormatAddress*>()))
            .WillOnce(Return(replyAddress));
        EXPECT_CALL(inMessage, strip(An<size_t>()));
        Proto::RequestHeader header;
        header.rooId = Proto::RooId(1, 1);
        header.requestId = Proto::RequestId{{{2, 2}, 3}, 0};
        Homa::unique_ptr<Homa::InMessage> request(&inMessage);
        handle = socket->taskPool.construct(socket, Proto::TaskId(42, 1),
                                            &header, std::move(request));
        task = &handle->task;
    }

    Mock::Homa::MockTransport transport;
    Mock::Homa::MockDriver driver;
    Mock::Homa::MockInMessage inMessage;
    Mock::Homa::MockOutMessage outMessage;
    Homa::Driver::Address replyAddress;
    SocketImpl* socket;
    SocketImpl::ServerTaskHandle* handle;
    ServerTaskImpl* task;
};

TEST_F(ServerTaskImplTest, constructor)
{
    Proto::RequestHeader header;
    header.rooId = Proto::RooId(1, 1);
    header.requestId = Proto::RequestId{{{2, 2}, 3}, 0};
    header.hasManifest = true;
    header.manifest.requestId = Proto::RequestId{{header.rooId, 0}, 0};
    Homa::unique_ptr<Homa::InMessage> request(&inMessage);

    EXPECT_CALL(transport, getDriver());
    EXPECT_CALL(driver,
                getAddress(TypedEq<const Homa::Driver::WireFormatAddress*>(
                    &header.replyAddress)))
        .WillOnce(Return(0xDEADBEEF));
    EXPECT_CALL(inMessage, strip(Eq(sizeof(Proto::RequestHeader))));

    ServerTaskImpl task(socket, Proto::TaskId(42, 1), &header,
                        std::move(request));
    EXPECT_EQ(socket, task.socket);
    EXPECT_EQ(header.rooId, task.rooId);
    EXPECT_EQ(header.requestId, task.requestId);
    EXPECT_TRUE(task.hasUnsentManifest);
    EXPECT_EQ(header.manifest.requestId, task.delegatedManifest.requestId);
    EXPECT_EQ(Proto::TaskId(42, 1), task.taskId);
    EXPECT_EQ(&inMessage, task.request.get());
    EXPECT_EQ(0xDEADBEEF, task.replyAddress);
    EXPECT_EQ(0, task.responseCount);
    EXPECT_EQ(0, task.requestCount);

    EXPECT_CALL(inMessage, release());
}

TEST_F(ServerTaskImplTest, getRequest)
{
    initDefaultTask();
    EXPECT_EQ(&inMessage, task->getRequest());
}

TEST_F(ServerTaskImplTest, reply)
{
    initDefaultTask();

    task->hasUnsentManifest = true;
    task->delegatedManifest.taskId = task->rooId;

    char* buffer[1024];

    EXPECT_EQ(0, task->requestCount);
    EXPECT_EQ(0, task->responseCount);
    EXPECT_TRUE(task->pendingMessages.empty());
    EXPECT_FALSE(task->bufferedMessage);

    EXPECT_CALL(transport, alloc())
        .WillOnce(
            Return(ByMove(Homa::unique_ptr<Homa::OutMessage>(&outMessage))));
    EXPECT_CALL(outMessage, reserve(Eq(sizeof(Proto::ResponseHeader))));
    EXPECT_CALL(outMessage, append(Eq(buffer), Eq(sizeof(buffer))));

    task->reply(buffer, sizeof(buffer));

    EXPECT_FALSE(task->bufferedMessageIsRequest);
    EXPECT_EQ(task->rooId, task->bufferedResponseHeader->rooId);
    EXPECT_EQ(task->requestId.branchId, task->bufferedResponseHeader->branchId);
    EXPECT_EQ(Proto::ResponseId(task->taskId, 0),
              task->bufferedResponseHeader->responseId);
    EXPECT_EQ(1, task->responseCount);
    EXPECT_TRUE(task->bufferedResponseHeader->hasManifest);
    EXPECT_EQ(task->delegatedManifest.taskId,
              task->bufferedResponseHeader->manifest.taskId);
    EXPECT_FALSE(task->hasUnsentManifest);
    EXPECT_EQ(replyAddress, task->bufferedMessageAddress);
    EXPECT_EQ(&outMessage, task->bufferedMessage.get());
    EXPECT_TRUE(task->pendingMessages.empty());

    EXPECT_CALL(outMessage, release());
}

TEST_F(ServerTaskImplTest, delegate)
{
    initDefaultTask();

    task->hasUnsentManifest = true;
    task->delegatedManifest.taskId = task->rooId;

    char* buffer[1024];

    EXPECT_EQ(0, task->requestCount);
    EXPECT_EQ(0, task->responseCount);
    EXPECT_TRUE(task->pendingMessages.empty());
    EXPECT_FALSE(task->bufferedMessage);

    EXPECT_CALL(transport, alloc())
        .WillOnce(
            Return(ByMove(Homa::unique_ptr<Homa::OutMessage>(&outMessage))));
    EXPECT_CALL(outMessage, reserve(Eq(sizeof(Proto::RequestHeader))));
    EXPECT_CALL(outMessage, append(Eq(buffer), Eq(sizeof(buffer))));
    EXPECT_CALL(transport, getDriver());
    EXPECT_CALL(driver,
                addressToWireFormat(Eq(replyAddress),
                                    An<Homa::Driver::WireFormatAddress*>()));

    task->delegate(0xFEED, buffer, sizeof(buffer));

    EXPECT_TRUE(task->bufferedMessageIsRequest);
    EXPECT_EQ(1, task->requestCount);
    EXPECT_EQ(task->rooId, task->bufferedRequestHeader->rooId);
    EXPECT_EQ(Proto::RequestId({task->taskId, 0}, 0),
              task->bufferedRequestHeader->requestId);
    EXPECT_TRUE(task->bufferedRequestHeader->hasManifest);
    EXPECT_EQ(task->delegatedManifest.taskId,
              task->bufferedRequestHeader->manifest.taskId);
    EXPECT_EQ(0xFEED, task->bufferedMessageAddress);
    EXPECT_EQ(&outMessage, task->bufferedMessage.get());
    EXPECT_TRUE(task->pendingMessages.empty());

    EXPECT_CALL(outMessage, release());
}

TEST_F(ServerTaskImplTest, poll)
{
    initDefaultTask();
    task->pendingMessages.push_back(
        Homa::unique_ptr<Homa::OutMessage>(&outMessage));
    task->pendingMessages.push_back(
        Homa::unique_ptr<Homa::OutMessage>(&outMessage));
    EXPECT_EQ(2U, task->pendingMessages.size());
    EXPECT_CALL(inMessage, dropped()).WillOnce(Return(false));
    EXPECT_CALL(outMessage, getStatus)
        .WillOnce(Return(Homa::OutMessage::Status::COMPLETED))
        .WillOnce(Return(Homa::OutMessage::Status::SENT));
    EXPECT_CALL(outMessage, release()).Times(2);
    EXPECT_TRUE(task->poll());
    EXPECT_EQ(1U, task->pendingMessages.size());
}

TEST_F(ServerTaskImplTest, poll_dropped)
{
    initDefaultTask();
    EXPECT_CALL(inMessage, dropped()).WillOnce(Return(true));
    EXPECT_FALSE(task->poll());
}

TEST_F(ServerTaskImplTest, poll_done)
{
    initDefaultTask();
    EXPECT_TRUE(task->pendingMessages.empty());
    EXPECT_CALL(inMessage, dropped()).WillOnce(Return(false));
    EXPECT_FALSE(task->poll());
}

TEST_F(ServerTaskImplTest, poll_failed)
{
    initDefaultTask();
    task->pendingMessages.push_back(
        Homa::unique_ptr<Homa::OutMessage>(&outMessage));
    EXPECT_CALL(inMessage, dropped()).WillOnce(Return(false));
    EXPECT_CALL(outMessage, getStatus)
        .WillOnce(Return(Homa::OutMessage::Status::FAILED));
    EXPECT_CALL(transport, alloc())
        .WillOnce(
            Return(ByMove(Homa::unique_ptr<Homa::OutMessage>(&outMessage))));
    EXPECT_CALL(outMessage, append(_, Eq(sizeof(Proto::ErrorHeader))));
    EXPECT_CALL(outMessage, send(Eq(task->replyAddress),
                                 Eq(Homa::OutMessage::NO_RETRY |
                                    Homa::OutMessage::NO_KEEP_ALIVE)));
    EXPECT_CALL(outMessage, release()).Times(2);

    EXPECT_FALSE(task->poll());
}

ACTION_P(SaveBlob, pointer)
{
    std::memcpy(pointer, arg0, arg1);
}

TEST_F(ServerTaskImplTest, handlePing)
{
    initDefaultTask();
    Proto::PongHeader pong;
    Homa::Driver::WireFormatAddress address[2];

    Proto::PingHeader header;
    header.requestId = task->requestId;
    Homa::unique_ptr<Homa::InMessage> message(&inMessage);

    Proto::RequestId delegatedRequestId({task->taskId, 0}, 0);

    task->pingInfo.destinations.push_back(0xABC);
    task->pingInfo.destinations.push_back(0xDEF);
    task->detached = true;
    task->requestCount = 3;
    task->responseCount = 8;
    task->pingInfo.pingCount = 0;

    InSequence s;

    // Expect address array creation
    EXPECT_CALL(transport, getDriver()).Times(1);
    EXPECT_CALL(driver, addressToWireFormat(
                            Eq(0xABC), An<Homa::Driver::WireFormatAddress*>()));
    EXPECT_CALL(transport, getDriver()).Times(1);
    EXPECT_CALL(driver, addressToWireFormat(
                            Eq(0xDEF), An<Homa::Driver::WireFormatAddress*>()));
    // Expect Pong
    EXPECT_CALL(transport, alloc())
        .WillOnce(
            Return(ByMove(Homa::unique_ptr<Homa::OutMessage>(&outMessage))));
    EXPECT_CALL(outMessage, append(_, Eq(sizeof(Proto::PongHeader))))
        .WillOnce(SaveBlob(&pong));
    EXPECT_CALL(outMessage, append(_, Eq(sizeof(address))))
        .WillOnce(SaveBlob(&address));
    EXPECT_CALL(outMessage, send(Eq(task->replyAddress),
                                 Eq(Homa::OutMessage::NO_RETRY |
                                    Homa::OutMessage::NO_KEEP_ALIVE)));
    EXPECT_CALL(outMessage, release());

    EXPECT_CALL(inMessage, release());

    task->handlePing(&header, std::move(message));

    EXPECT_EQ(1, task->pingInfo.pingCount);
    EXPECT_EQ(task->rooId, pong.rooId);
    EXPECT_EQ(task->requestId, pong.requestId);
    EXPECT_EQ(task->taskId, pong.taskId);
    EXPECT_EQ(2, pong.requestCount);
    EXPECT_EQ(8, pong.responseCount);
    EXPECT_TRUE(pong.taskComplete);
    EXPECT_TRUE(pong.branchComplete);
}

TEST_F(ServerTaskImplTest, handleTimeout)
{
    initDefaultTask();
    task->pingInfo.pingCount = 1;
    EXPECT_FALSE(task->detached.load());
    EXPECT_TRUE(task->handleTimeout());
    EXPECT_EQ(1, task->pingInfo.pingCount);
    task->detached = true;
    EXPECT_TRUE(task->handleTimeout());
    EXPECT_EQ(0, task->pingInfo.pingCount);
    EXPECT_FALSE(task->handleTimeout());
}

TEST_F(ServerTaskImplTest, destroy_noMessages)
{
    initDefaultTask();

    task->hasUnsentManifest = true;

    EXPECT_TRUE(task->pendingMessages.empty());
    EXPECT_FALSE(task->detached);

    EXPECT_CALL(transport, alloc())
        .WillOnce(
            Return(ByMove(Homa::unique_ptr<Homa::OutMessage>(&outMessage))));
    EXPECT_CALL(transport, getDriver()).Times(2);
    EXPECT_CALL(driver, getLocalAddress()).WillOnce(Return(0xFEED));
    EXPECT_CALL(driver,
                addressToWireFormat(Eq(0xFEED),
                                    An<Homa::Driver::WireFormatAddress*>()));
    EXPECT_CALL(outMessage, append(_, Eq(sizeof(Proto::ManifestHeader))));
    EXPECT_CALL(outMessage, append(_, Eq(sizeof(Proto::Manifest))));
    EXPECT_CALL(outMessage, append(Eq(&task->delegatedManifest),
                                   Eq(sizeof(Proto::Manifest))));
    EXPECT_CALL(outMessage,
                send(Eq(replyAddress), Eq(Homa::OutMessage::NO_RETRY |
                                          Homa::OutMessage::NO_KEEP_ALIVE)));

    task->destroy();

    EXPECT_FALSE(task->pendingMessages.empty());
    EXPECT_TRUE(task->detached);

    EXPECT_CALL(outMessage, release());
}

TEST_F(ServerTaskImplTest, destroy_request_single)
{
    initDefaultTask();

    Homa::unique_ptr<Homa::OutMessage> message(&outMessage);

    task->bufferedMessage = std::move(message);
    task->bufferedMessageIsRequest = true;
    task->bufferedMessageAddress = 0xFEED;
    task->requestCount = 1;

    EXPECT_TRUE(task->pendingMessages.empty());
    EXPECT_FALSE(task->detached);

    EXPECT_CALL(outMessage, length());
    EXPECT_CALL(outMessage, prepend(Eq(task->bufferedRequestHeader),
                                    Eq(sizeof(Proto::RequestHeader))));
    EXPECT_CALL(outMessage,
                send(Eq(0xFEED), Eq(Homa::OutMessage::NO_RETRY |
                                    Homa::OutMessage::NO_KEEP_ALIVE)));

    task->destroy();

    EXPECT_EQ(Proto::RequestId(task->requestId.branchId,
                               task->requestId.sequence + 1),
              task->bufferedRequestHeader->requestId);
    EXPECT_EQ(&outMessage, task->pendingMessages.back().get());
    EXPECT_TRUE(task->detached);

    EXPECT_CALL(outMessage, release());
}

TEST_F(ServerTaskImplTest, destroy_response_single)
{
    initDefaultTask();

    Homa::unique_ptr<Homa::OutMessage> message(&outMessage);

    task->bufferedMessage = std::move(message);
    task->bufferedMessageIsRequest = false;
    task->bufferedMessageAddress = replyAddress;
    task->responseCount = 1;

    EXPECT_TRUE(task->pendingMessages.empty());
    EXPECT_FALSE(task->detached);

    EXPECT_CALL(outMessage, length());
    EXPECT_CALL(outMessage, prepend(Eq(task->bufferedResponseHeader),
                                    Eq(sizeof(Proto::ResponseHeader))));
    EXPECT_CALL(outMessage,
                send(Eq(replyAddress), Eq(Homa::OutMessage::NO_RETRY |
                                          Homa::OutMessage::NO_KEEP_ALIVE)));

    task->destroy();

    EXPECT_TRUE(task->bufferedResponseHeader->manifestImplied);
    EXPECT_EQ(&outMessage, task->pendingMessages.back().get());
    EXPECT_TRUE(task->detached);

    EXPECT_CALL(outMessage, release());
}

TEST_F(ServerTaskImplTest, destroy_request_multiple)
{
    initDefaultTask();

    Homa::unique_ptr<Homa::OutMessage> message(&outMessage);

    task->bufferedMessage = std::move(message);
    task->bufferedMessageIsRequest = true;
    task->bufferedMessageAddress = 0xFEED;
    task->bufferedRequestHeader->requestId =
        Proto::RequestId{{task->taskId, 1}, 0};
    task->requestCount = 2;
    task->responseCount = 1;
    task->pingInfo.destinations.push_back(0xDEAD);

    EXPECT_TRUE(task->pendingMessages.empty());
    EXPECT_FALSE(task->detached);

    EXPECT_CALL(transport, getDriver()).Times(2);
    EXPECT_CALL(driver, getLocalAddress()).WillOnce(Return(0xFEED));
    EXPECT_CALL(driver,
                addressToWireFormat(Eq(0xFEED),
                                    An<Homa::Driver::WireFormatAddress*>()));
    EXPECT_CALL(outMessage, length());
    EXPECT_CALL(outMessage, prepend(Eq(task->bufferedRequestHeader),
                                    Eq(sizeof(Proto::RequestHeader))));
    EXPECT_CALL(outMessage,
                send(Eq(0xFEED), Eq(Homa::OutMessage::NO_RETRY |
                                    Homa::OutMessage::NO_KEEP_ALIVE)));

    task->destroy();

    EXPECT_TRUE(task->bufferedRequestHeader->hasManifest);
    EXPECT_EQ(task->requestId, task->bufferedRequestHeader->manifest.requestId);
    EXPECT_EQ(task->taskId, task->bufferedRequestHeader->manifest.taskId);
    EXPECT_EQ(task->requestCount,
              task->bufferedRequestHeader->manifest.requestCount);
    EXPECT_EQ(task->responseCount,
              task->bufferedRequestHeader->manifest.responseCount);
    EXPECT_EQ(&outMessage, task->pendingMessages.back().get());
    EXPECT_TRUE(task->detached);

    EXPECT_CALL(outMessage, release());
}

TEST_F(ServerTaskImplTest, destroy_response_multiple)
{
    initDefaultTask();

    Homa::unique_ptr<Homa::OutMessage> message(&outMessage);

    task->bufferedMessage = std::move(message);
    task->bufferedMessageIsRequest = false;
    task->bufferedMessageAddress = replyAddress;
    task->requestCount = 1;
    task->responseCount = 2;

    EXPECT_TRUE(task->pendingMessages.empty());
    EXPECT_FALSE(task->detached);

    EXPECT_CALL(transport, getDriver()).Times(2);
    EXPECT_CALL(driver, getLocalAddress()).WillOnce(Return(0xFEED));
    EXPECT_CALL(driver,
                addressToWireFormat(Eq(0xFEED),
                                    An<Homa::Driver::WireFormatAddress*>()));
    EXPECT_CALL(outMessage, length());
    EXPECT_CALL(outMessage, prepend(Eq(task->bufferedResponseHeader),
                                    Eq(sizeof(Proto::ResponseHeader))));
    EXPECT_CALL(outMessage,
                send(Eq(replyAddress), Eq(Homa::OutMessage::NO_RETRY |
                                          Homa::OutMessage::NO_KEEP_ALIVE)));

    task->destroy();

    EXPECT_TRUE(task->bufferedResponseHeader->hasManifest);
    EXPECT_EQ(task->requestId,
              task->bufferedResponseHeader->manifest.requestId);
    EXPECT_EQ(task->taskId, task->bufferedResponseHeader->manifest.taskId);
    EXPECT_EQ(task->requestCount,
              task->bufferedResponseHeader->manifest.requestCount);
    EXPECT_EQ(task->responseCount,
              task->bufferedResponseHeader->manifest.responseCount);
    EXPECT_EQ(&outMessage, task->pendingMessages.back().get());
    EXPECT_TRUE(task->detached);

    EXPECT_CALL(outMessage, release());
}

TEST_F(ServerTaskImplTest, sendBufferedMessage_request)
{
    initDefaultTask();

    Homa::unique_ptr<Homa::OutMessage> message(&outMessage);

    task->bufferedMessage = std::move(message);
    task->bufferedMessageIsRequest = true;
    task->bufferedMessageAddress = 0xFEED;
    task->bufferedRequestHeader->requestId =
        Proto::RequestId{{task->taskId, 0}, 0};

    EXPECT_EQ(0, task->requestCount);
    EXPECT_EQ(0, task->responseCount);
    EXPECT_TRUE(task->pingInfo.destinations.empty());
    EXPECT_TRUE(task->pendingMessages.empty());

    EXPECT_CALL(outMessage, length());
    EXPECT_CALL(outMessage, prepend(Eq(task->bufferedRequestHeader),
                                    Eq(sizeof(Proto::RequestHeader))));
    EXPECT_CALL(outMessage,
                send(Eq(0xFEED), Eq(Homa::OutMessage::NO_RETRY |
                                    Homa::OutMessage::NO_KEEP_ALIVE)));

    task->sendBufferedMessage();

    EXPECT_EQ(0xFEED, task->pingInfo.destinations.back());
    EXPECT_EQ(&outMessage, task->pendingMessages.back().get());

    EXPECT_CALL(outMessage, release());
}

TEST_F(ServerTaskImplTest, sendBufferedMessage_response)
{
    initDefaultTask();

    Homa::unique_ptr<Homa::OutMessage> message(&outMessage);

    task->bufferedMessage = std::move(message);
    task->bufferedMessageIsRequest = false;
    task->bufferedMessageAddress = replyAddress;

    EXPECT_EQ(0, task->requestCount);
    EXPECT_EQ(0, task->responseCount);
    EXPECT_TRUE(task->pendingMessages.empty());

    EXPECT_CALL(outMessage, length());
    EXPECT_CALL(outMessage, prepend(Eq(task->bufferedResponseHeader),
                                    Eq(sizeof(Proto::ResponseHeader))));
    EXPECT_CALL(outMessage,
                send(Eq(replyAddress), Eq(Homa::OutMessage::NO_RETRY |
                                          Homa::OutMessage::NO_KEEP_ALIVE)));

    task->sendBufferedMessage();

    EXPECT_EQ(&outMessage, task->pendingMessages.back().get());

    EXPECT_CALL(outMessage, release());
}

}  // namespace
}  // namespace Roo
