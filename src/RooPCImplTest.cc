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

#include <Roo/Debug.h>
#include <gtest/gtest.h>

#include "Mock/MockHoma.h"
#include "RooPCImpl.h"
#include "SocketImpl.h"

namespace Roo {
namespace {

using ::testing::An;
using ::testing::ByMove;
using ::testing::Eq;
using ::testing::Return;

class RooPCImplTest : public ::testing::Test {
  public:
    RooPCImplTest()
        : transport()
        , driver()
        , inMessage()
        , outMessage()
        , rooId(42, 1)
        , replyAddress(0xDEADBEEF)
        , socket(nullptr)
        , rpc(nullptr)
    {
        ON_CALL(transport, getId()).WillByDefault(Return(42));
        ON_CALL(transport, getDriver()).WillByDefault(Return(&driver));

        // Setup default socket
        EXPECT_CALL(transport, getId());
        socket = new SocketImpl(&transport);

        // Setup default RooPC
        rpc = new RooPCImpl(socket, rooId);
    }

    ~RooPCImplTest()
    {
        delete rpc;
        delete socket;
    }

    Mock::Homa::MockTransport transport;
    Mock::Homa::MockDriver driver;
    Mock::Homa::MockInMessage inMessage;
    Mock::Homa::MockOutMessage outMessage;
    Proto::RooId rooId;
    Homa::Driver::Address replyAddress;
    SocketImpl* socket;
    RooPCImpl* rpc;
};

struct VectorHandler {
    VectorHandler()
        : messages()
    {}
    void operator()(Debug::DebugMessage message)
    {
        messages.push_back(message);
    }
    std::vector<Debug::DebugMessage> messages;
};

TEST_F(RooPCImplTest, constructor)
{
    RooPCImpl rpc(socket, rooId);
    EXPECT_EQ(socket, rpc.socket);
    EXPECT_EQ(rooId, rpc.rooId);
}

TEST_F(RooPCImplTest, allocRequest)
{
    EXPECT_CALL(transport, alloc())
        .WillOnce(
            Return(ByMove(Homa::unique_ptr<Homa::OutMessage>(&outMessage))));
    EXPECT_CALL(outMessage, reserve(Eq(sizeof(Proto::RequestHeader))));

    Homa::unique_ptr<Homa::OutMessage> message = rpc->allocRequest();

    EXPECT_EQ(&outMessage, message.get());
    EXPECT_CALL(outMessage, release());
}

TEST_F(RooPCImplTest, send)
{
    Homa::unique_ptr<Homa::OutMessage> message(&outMessage);

    EXPECT_EQ(0, rpc->requestCount);
    EXPECT_TRUE(rpc->tasks.find(Proto::BranchId(rooId, 0)) == rpc->tasks.end());
    EXPECT_EQ(0U, rpc->manifestsOutstanding);
    EXPECT_TRUE(rpc->pendingRequests.empty());

    EXPECT_CALL(transport, getDriver()).Times(2);
    EXPECT_CALL(driver, getLocalAddress()).WillOnce(Return(replyAddress));
    EXPECT_CALL(driver,
                addressToWireFormat(Eq(replyAddress),
                                    An<Homa::Driver::WireFormatAddress*>()));
    EXPECT_CALL(outMessage,
                prepend(An<const void*>(), Eq(sizeof(Proto::RequestHeader))));
    EXPECT_CALL(outMessage, send(Eq(0xFEED)));

    rpc->send(0xFEED, std::move(message));

    EXPECT_EQ(1, rpc->requestCount);
    EXPECT_TRUE(rpc->tasks.find(Proto::BranchId(rooId, 0)) != rpc->tasks.end());
    EXPECT_EQ(1U, rpc->manifestsOutstanding);
    EXPECT_FALSE(rpc->pendingRequests.empty());

    EXPECT_CALL(outMessage, release());
}

TEST_F(RooPCImplTest, receive)
{
    rpc->responseQueue.push_back(
        std::move(Homa::unique_ptr<Homa::InMessage>(&inMessage)));
    {
        Homa::unique_ptr<Homa::InMessage> message = rpc->receive();
        EXPECT_EQ(&inMessage, message.get());
        EXPECT_CALL(inMessage, release());
    }
    {
        Homa::unique_ptr<Homa::InMessage> message = rpc->receive();
        EXPECT_FALSE(message);
    }
}

TEST_F(RooPCImplTest, checkStatus)
{
    EXPECT_EQ(RooPC::Status::NOT_STARTED, rpc->checkStatus());

    rpc->pendingRequests.push_back(
        std::move(Homa::unique_ptr<Homa::OutMessage>(&outMessage)));
    rpc->requestCount = 1;
    EXPECT_EQ(RooPC::Status::COMPLETED, rpc->checkStatus());

    rpc->manifestsOutstanding = 1;
    EXPECT_CALL(outMessage, getStatus())
        .WillOnce(Return(Homa::OutMessage::Status::FAILED));
    EXPECT_EQ(RooPC::Status::FAILED, rpc->checkStatus());

    EXPECT_CALL(outMessage, getStatus())
        .WillOnce(Return(Homa::OutMessage::Status::SENT));
    EXPECT_EQ(RooPC::Status::IN_PROGRESS, rpc->checkStatus());

    EXPECT_CALL(outMessage, release());
}

TEST_F(RooPCImplTest, wait)
{
    // nothing to test
    rpc->wait();
}

TEST_F(RooPCImplTest, destroy)
{
    // nothing to test
}

TEST_F(RooPCImplTest, handleResponse_unexpected)
{
    Proto::BranchId branchId(Proto::TaskId(1, 1), 1);
    Proto::ResponseId responseId(Proto::TaskId(2, 2), 2);
    Proto::ResponseHeader header;
    header.branchId = branchId;
    header.responseId = responseId;
    Homa::unique_ptr<Homa::InMessage> message(&inMessage);
    rpc->pendingRequests.push_back(
        std::move(Homa::unique_ptr<Homa::OutMessage>(&outMessage)));
    EXPECT_CALL(inMessage, acknowledge());
    EXPECT_CALL(inMessage, strip(Eq(sizeof(Proto::ResponseHeader))));

    EXPECT_EQ(0, rpc->expectedResponses.count(responseId));
    EXPECT_EQ(0, rpc->tasks.count(branchId));
    EXPECT_EQ(0, rpc->manifestsOutstanding);

    rpc->handleResponse(&header, std::move(message));

    EXPECT_EQ(1, rpc->expectedResponses.count(responseId));
    EXPECT_TRUE(rpc->expectedResponses.at(responseId));
    EXPECT_EQ(1, rpc->tasks.count(branchId));
    EXPECT_FALSE(rpc->tasks.at(branchId));
    EXPECT_EQ(1, rpc->manifestsOutstanding);
    EXPECT_FALSE(rpc->pendingRequests.empty());

    EXPECT_CALL(outMessage, release());
    EXPECT_CALL(inMessage, release());
}

TEST_F(RooPCImplTest, handleResponse_expected)
{
    Proto::ResponseId responseId(Proto::TaskId(2, 2), 2);
    Proto::ResponseHeader header;
    header.responseId = responseId;
    Homa::unique_ptr<Homa::InMessage> message(&inMessage);
    rpc->pendingRequests.push_back(
        std::move(Homa::unique_ptr<Homa::OutMessage>(&outMessage)));
    EXPECT_CALL(inMessage, acknowledge());
    EXPECT_CALL(inMessage, strip(Eq(sizeof(Proto::ResponseHeader))));
    EXPECT_CALL(outMessage, release());

    rpc->expectedResponses[responseId] = false;
    rpc->responsesOutstanding = 1;
    EXPECT_EQ(0, rpc->responseQueue.size());

    rpc->handleResponse(&header, std::move(message));

    EXPECT_TRUE(rpc->expectedResponses.at(responseId));
    EXPECT_EQ(0, rpc->responsesOutstanding);
    EXPECT_EQ(1, rpc->responseQueue.size());
    EXPECT_TRUE(rpc->pendingRequests.empty());

    EXPECT_CALL(inMessage, release());
}

TEST_F(RooPCImplTest, handleResponse_duplicate)
{
    Proto::ResponseId responseId(Proto::TaskId(2, 2), 2);
    Proto::ResponseHeader header;
    header.responseId = responseId;
    Homa::unique_ptr<Homa::InMessage> message(&inMessage);
    EXPECT_CALL(inMessage, acknowledge());
    EXPECT_CALL(inMessage, strip(Eq(sizeof(Proto::ResponseHeader))));
    EXPECT_CALL(inMessage, release());

    rpc->expectedResponses[responseId] = true;
    EXPECT_EQ(0, rpc->responsesOutstanding);
    EXPECT_EQ(0, rpc->responseQueue.size());

    VectorHandler handler;
    Debug::setLogHandler(std::ref(handler));

    rpc->handleResponse(&header, std::move(message));

    EXPECT_EQ(1U, handler.messages.size());
    const Debug::DebugMessage& m = handler.messages.at(0);
    EXPECT_STREQ("src/RooPCImpl.cc", m.filename);
    EXPECT_STREQ("handleResponse", m.function);
    EXPECT_EQ(int(Debug::LogLevel::NOTICE), m.logLevel);
    EXPECT_EQ("Duplicate response received for RooPC (42, 1)", m.message);
    Debug::setLogHandler(std::function<void(Debug::DebugMessage)>());

    EXPECT_TRUE(rpc->expectedResponses.at(responseId));
    EXPECT_EQ(0, rpc->responsesOutstanding);
    EXPECT_EQ(0, rpc->responseQueue.size());
}

TEST_F(RooPCImplTest, handleManifest_basic)
{
    Proto::BranchId branchId(Proto::TaskId(1, 1), 1);
    Proto::TaskId taskId(2, 2);
    Proto::Manifest manifest;
    manifest.branchId = branchId;
    manifest.taskId = taskId;
    manifest.requestCount = 2;
    manifest.responseCount = 2;
    Homa::unique_ptr<Homa::InMessage> message(&inMessage);

    rpc->tasks[Proto::BranchId(taskId, 0)] = true;
    rpc->expectedResponses[Proto::ResponseId(taskId, 0)] = true;

    rpc->pendingRequests.push_back(
        std::move(Homa::unique_ptr<Homa::OutMessage>(&outMessage)));

    EXPECT_EQ(1, rpc->tasks.size());
    EXPECT_EQ(1, rpc->expectedResponses.size());

    EXPECT_CALL(inMessage, acknowledge());
    EXPECT_CALL(inMessage, release());

    rpc->handleManifest(&manifest, std::move(message));

    EXPECT_EQ(1, rpc->manifestsOutstanding);
    EXPECT_EQ(1, rpc->responsesOutstanding);

    EXPECT_EQ(3, rpc->tasks.size());
    EXPECT_TRUE(rpc->tasks.at(Proto::BranchId(taskId, 0)));
    EXPECT_FALSE(rpc->tasks.at(Proto::BranchId(taskId, 1)));
    EXPECT_TRUE(rpc->tasks.at(branchId));

    EXPECT_EQ(2, rpc->expectedResponses.size());
    EXPECT_TRUE(rpc->expectedResponses.at(Proto::ResponseId(taskId, 0)));
    EXPECT_FALSE(rpc->expectedResponses.at(Proto::ResponseId(taskId, 1)));

    EXPECT_FALSE(rpc->pendingRequests.empty());

    EXPECT_CALL(outMessage, release());
}

TEST_F(RooPCImplTest, handleManifest_tracked)
{
    Proto::BranchId branchId(Proto::TaskId(1, 1), 1);
    Proto::Manifest manifest;
    manifest.branchId = branchId;
    Homa::unique_ptr<Homa::InMessage> message(&inMessage);

    rpc->tasks[branchId] = false;
    rpc->manifestsOutstanding = 1;

    rpc->pendingRequests.push_back(
        std::move(Homa::unique_ptr<Homa::OutMessage>(&outMessage)));

    EXPECT_CALL(inMessage, acknowledge());
    EXPECT_CALL(inMessage, release());
    EXPECT_CALL(outMessage, release());

    rpc->handleManifest(&manifest, std::move(message));

    EXPECT_EQ(0, rpc->manifestsOutstanding);
    EXPECT_TRUE(rpc->tasks.at(branchId));

    EXPECT_TRUE(rpc->pendingRequests.empty());
}

TEST_F(RooPCImplTest, handleManifest_duplicate)
{
    Proto::BranchId branchId(Proto::TaskId(1, 1), 1);
    Proto::Manifest manifest;
    manifest.branchId = branchId;
    Homa::unique_ptr<Homa::InMessage> message(&inMessage);

    rpc->tasks[branchId] = true;

    EXPECT_CALL(inMessage, acknowledge());
    EXPECT_CALL(inMessage, release());

    VectorHandler handler;
    Debug::setLogHandler(std::ref(handler));

    rpc->handleManifest(&manifest, std::move(message));

    EXPECT_EQ(1U, handler.messages.size());
    const Debug::DebugMessage& m = handler.messages.at(0);
    EXPECT_STREQ("src/RooPCImpl.cc", m.filename);
    EXPECT_STREQ("handleManifest", m.function);
    EXPECT_EQ(int(Debug::LogLevel::WARNING), m.logLevel);
    EXPECT_EQ("Duplicate Manifest received for RooPC (42, 1)", m.message);
    Debug::setLogHandler(std::function<void(Debug::DebugMessage)>());
}

}  // namespace
}  // namespace Roo
