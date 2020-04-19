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

#include "Mock/MockHoma.h"
#include "ServerTaskImpl.h"
#include "SocketImpl.h"

namespace Roo {
namespace {

using ::testing::_;
using ::testing::An;
using ::testing::ByMove;
using ::testing::Eq;
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
        if (task != nullptr) {
            EXPECT_CALL(inMessage, release());
            delete task;
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
        header.branchId = Proto::BranchId(Proto::RooId(2, 2), 3);
        Homa::unique_ptr<Homa::InMessage> request(&inMessage);
        task = new ServerTaskImpl(socket, &header, std::move(request));
    }

    Mock::Homa::MockTransport transport;
    Mock::Homa::MockDriver driver;
    Mock::Homa::MockInMessage inMessage;
    Mock::Homa::MockOutMessage outMessage;
    Homa::Driver::Address replyAddress;
    SocketImpl* socket;
    ServerTaskImpl* task;
};

TEST_F(ServerTaskImplTest, constructor)
{
    Proto::RequestHeader header;
    header.rooId = Proto::RooId(1, 1);
    header.branchId = Proto::BranchId(Proto::RooId(2, 2), 3);
    Homa::unique_ptr<Homa::InMessage> request(&inMessage);

    EXPECT_CALL(transport, getDriver());
    EXPECT_CALL(driver,
                getAddress(TypedEq<const Homa::Driver::WireFormatAddress*>(
                    &header.replyAddress)))
        .WillOnce(Return(0xDEADBEEF));
    EXPECT_CALL(inMessage, strip(Eq(sizeof(Proto::RequestHeader))));

    ServerTaskImpl task(socket, &header, std::move(request));
    EXPECT_EQ(socket, task.socket);
    EXPECT_EQ(header.rooId, task.rooId);
    EXPECT_EQ(header.branchId, task.branchId);
    EXPECT_EQ(Proto::TaskId(42, 1), task.taskId);
    EXPECT_FALSE(task.isInitialRequest);
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

TEST_F(ServerTaskImplTest, allocOutMessage)
{
    initDefaultTask();

    EXPECT_CALL(transport, alloc())
        .WillOnce(
            Return(ByMove(Homa::unique_ptr<Homa::OutMessage>(&outMessage))));
    EXPECT_CALL(outMessage, reserve(Eq(sizeof(Proto::RequestHeader))));

    Homa::unique_ptr<Homa::OutMessage> message = task->allocOutMessage();

    EXPECT_EQ(&outMessage, message.get());
    EXPECT_CALL(outMessage, release());
}

TEST_F(ServerTaskImplTest, reply)
{
    initDefaultTask();

    Homa::unique_ptr<Homa::OutMessage> message(&outMessage);

    EXPECT_EQ(0, task->responseCount);
    EXPECT_TRUE(task->pendingMessages.empty());
    EXPECT_TRUE(task->outboundMessages.empty());

    EXPECT_CALL(outMessage,
                prepend(An<const void*>(), Eq(sizeof(Proto::ResponseHeader))));
    EXPECT_CALL(outMessage, send(Eq(replyAddress)));

    task->reply(std::move(message));

    EXPECT_EQ(1, task->responseCount);
    EXPECT_FALSE(task->pendingMessages.empty());
    EXPECT_FALSE(task->outboundMessages.empty());

    EXPECT_CALL(outMessage, release());
}

TEST_F(ServerTaskImplTest, delegate)
{
    initDefaultTask();

    Homa::unique_ptr<Homa::OutMessage> message(&outMessage);

    EXPECT_EQ(0, task->requestCount);
    EXPECT_TRUE(task->pendingMessages.empty());
    EXPECT_TRUE(task->outboundMessages.empty());

    EXPECT_CALL(transport, getDriver());
    EXPECT_CALL(driver,
                addressToWireFormat(Eq(replyAddress),
                                    An<Homa::Driver::WireFormatAddress*>()));
    EXPECT_CALL(outMessage,
                prepend(An<const void*>(), Eq(sizeof(Proto::RequestHeader))));
    EXPECT_CALL(outMessage, send(Eq(0xFEED)));

    task->delegate(0xFEED, std::move(message));

    EXPECT_EQ(1, task->requestCount);
    EXPECT_FALSE(task->pendingMessages.empty());
    EXPECT_FALSE(task->outboundMessages.empty());

    EXPECT_CALL(outMessage, release());
}

TEST_F(ServerTaskImplTest, poll)
{
    initDefaultTask();
    task->pendingMessages.push_back(&outMessage);
    task->pendingMessages.push_back(&outMessage);
    EXPECT_EQ(2U, task->pendingMessages.size());
    EXPECT_CALL(inMessage, dropped()).WillOnce(Return(false));
    EXPECT_CALL(outMessage, getStatus)
        .WillOnce(Return(Homa::OutMessage::Status::COMPLETED))
        .WillOnce(Return(Homa::OutMessage::Status::SENT));
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
    EXPECT_FALSE(task->isInitialRequest);
    EXPECT_CALL(inMessage, dropped()).WillOnce(Return(false));
    EXPECT_CALL(inMessage, acknowledge());
    EXPECT_FALSE(task->poll());
}

TEST_F(ServerTaskImplTest, poll_failed)
{
    initDefaultTask();
    task->pendingMessages.push_back(&outMessage);
    EXPECT_CALL(inMessage, dropped()).WillOnce(Return(false));
    EXPECT_CALL(outMessage, getStatus)
        .WillOnce(Return(Homa::OutMessage::Status::FAILED));
    EXPECT_CALL(inMessage, fail());
    EXPECT_FALSE(task->poll());
}

TEST_F(ServerTaskImplTest, destroy)
{
    initDefaultTask();

    EXPECT_TRUE(task->pendingMessages.empty());
    EXPECT_TRUE(task->outboundMessages.empty());
    EXPECT_FALSE(task->detached);

    EXPECT_CALL(transport, alloc())
        .WillOnce(
            Return(ByMove(Homa::unique_ptr<Homa::OutMessage>(&outMessage))));
    EXPECT_CALL(outMessage, append(_, Eq(sizeof(Proto::Manifest))));
    EXPECT_CALL(outMessage, send(Eq(replyAddress)));

    task->destroy();

    EXPECT_FALSE(task->pendingMessages.empty());
    EXPECT_FALSE(task->outboundMessages.empty());
    EXPECT_TRUE(task->detached);

    EXPECT_CALL(outMessage, release());
}

}  // namespace
}  // namespace Roo
