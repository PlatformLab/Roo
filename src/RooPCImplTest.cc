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

using ::testing::_;
using ::testing::An;
using ::testing::ByMove;
using ::testing::Eq;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::TypedEq;

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

TEST_F(RooPCImplTest, send)
{
    char* buffer[1024];
    Proto::RequestId requestId{{rooId, 0}, 0};

    EXPECT_EQ(0, rpc->requestCount);
    EXPECT_TRUE(rpc->branches.find(requestId.branchId) == rpc->branches.end());
    EXPECT_EQ(0U, rpc->manifestsOutstanding);
    EXPECT_TRUE(rpc->pendingRequests.empty());

    EXPECT_CALL(transport, alloc())
        .WillOnce(
            Return(ByMove(Homa::unique_ptr<Homa::OutMessage>(&outMessage))));
    EXPECT_CALL(transport, getDriver()).Times(2);
    EXPECT_CALL(driver, getLocalAddress()).WillOnce(Return(replyAddress));
    EXPECT_CALL(driver,
                addressToWireFormat(Eq(replyAddress),
                                    An<Homa::Driver::WireFormatAddress*>()));
    EXPECT_CALL(outMessage,
                append(An<const void*>(), Eq(sizeof(Proto::RequestHeader))));
    EXPECT_CALL(outMessage, append(buffer, Eq(sizeof(buffer))));
    EXPECT_CALL(outMessage,
                send(Eq(0xFEED), Eq(Homa::OutMessage::NO_RETRY |
                                    Homa::OutMessage::NO_KEEP_ALIVE)));

    rpc->send(0xFEED, buffer, sizeof(buffer));

    EXPECT_EQ(1, rpc->requestCount);
    ASSERT_TRUE(rpc->branches.find(requestId.branchId) != rpc->branches.end());
    EXPECT_FALSE(rpc->branches.at(requestId.branchId).complete);
    EXPECT_EQ(requestId, rpc->branches.at(requestId.branchId).pingReceiverId);
    EXPECT_EQ(0xFEED, rpc->branches.at(requestId.branchId).pingAddress);
    EXPECT_EQ(0, rpc->branches.at(requestId.branchId).pingTimeouts);
    EXPECT_EQ(1U, rpc->manifestsOutstanding);
    EXPECT_FALSE(rpc->pendingRequests.empty());

    EXPECT_CALL(outMessage, release());
}

TEST_F(RooPCImplTest, receive)
{
    Homa::InMessage* message = nullptr;

    rpc->responseQueue.push_back(&inMessage);

    message = rpc->receive();
    EXPECT_EQ(&inMessage, message);

    message = rpc->receive();
    EXPECT_EQ(nullptr, message);
}

TEST_F(RooPCImplTest, checkStatus)
{
    EXPECT_EQ(RooPC::Status::NOT_STARTED, rpc->checkStatus());

    rpc->pendingRequests.push_back(
        std::move(Homa::unique_ptr<Homa::OutMessage>(&outMessage)));
    rpc->requestCount = 1;
    EXPECT_EQ(RooPC::Status::COMPLETED, rpc->checkStatus());

    rpc->manifestsOutstanding = 1;
    rpc->error = true;
    EXPECT_EQ(RooPC::Status::FAILED, rpc->checkStatus());

    rpc->error = false;
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

TEST_F(RooPCImplTest, handleResponse_basic)
{
    Proto::RooId rooId(0, 0);
    Proto::BranchId rootId(rooId, 0);
    Proto::RequestId requestId(rootId, 0);
    Proto::BranchId branchId(Proto::TaskId(1, 1), 1);
    Proto::ResponseId responseId(Proto::TaskId(2, 2), 0);
    Proto::ResponseHeader header;
    header.branchId = branchId;
    header.responseId = responseId;
    header.manifestImplied = true;
    header.hasManifest = true;
    header.manifest = Proto::Manifest(requestId, branchId.taskId, 2, 0);

    rpc->branches[rootId] = {false, {}, {}, 0};
    rpc->branches[Proto::BranchId(branchId.taskId, 0)] = {true, {}, {}, 0};
    rpc->manifestsOutstanding = 1;

    Homa::unique_ptr<Homa::InMessage> message(&inMessage);
    rpc->pendingRequests.push_back(
        std::move(Homa::unique_ptr<Homa::OutMessage>(&outMessage)));
    EXPECT_CALL(inMessage, strip(Eq(sizeof(Proto::ResponseHeader))));
    EXPECT_CALL(transport, getDriver());
    EXPECT_CALL(driver,
                getAddress(TypedEq<const Homa::Driver::WireFormatAddress*>(
                    &header.manifest.serverAddress)))
        .WillOnce(Return(0xFEED));
    EXPECT_CALL(outMessage, release());

    EXPECT_EQ(0, rpc->expectedResponses.count(responseId));
    EXPECT_EQ(2, rpc->branches.size());
    EXPECT_EQ(1, rpc->manifestsOutstanding);
    EXPECT_EQ(0, rpc->responsesOutstanding);

    rpc->handleResponse(&header, std::move(message));

    EXPECT_TRUE(rpc->branches.at(branchId).complete);
    EXPECT_TRUE(rpc->branches.at(rootId).complete);
    EXPECT_TRUE(rpc->expectedResponses.at(responseId));
    EXPECT_EQ(0, rpc->manifestsOutstanding);
    EXPECT_EQ(0, rpc->responsesOutstanding);
    EXPECT_TRUE(rpc->pendingRequests.empty());

    EXPECT_CALL(inMessage, release());
}

TEST_F(RooPCImplTest, handleResponse_unexpected)
{
    Proto::RooId rooId(0, 0);
    Proto::BranchId rootId(rooId, 0);
    Proto::BranchId branchId(Proto::TaskId(1, 1), 1);
    Proto::ResponseId responseId(Proto::TaskId(2, 2), 2);
    Proto::ResponseHeader header;
    header.branchId = branchId;
    header.responseId = responseId;
    Homa::unique_ptr<Homa::InMessage> message(&inMessage);
    rpc->pendingRequests.push_back(
        std::move(Homa::unique_ptr<Homa::OutMessage>(&outMessage)));
    rpc->branches[rootId] = {false, {}, {}, 0};
    rpc->manifestsOutstanding = 1;

    EXPECT_CALL(inMessage, strip(Eq(sizeof(Proto::ResponseHeader))));

    EXPECT_EQ(0, rpc->expectedResponses.count(responseId));
    EXPECT_EQ(1, rpc->branches.size());
    EXPECT_EQ(1, rpc->manifestsOutstanding);

    rpc->handleResponse(&header, std::move(message));

    EXPECT_EQ(1, rpc->expectedResponses.count(responseId));
    EXPECT_TRUE(rpc->expectedResponses.at(responseId));
    EXPECT_EQ(1, rpc->branches.size());
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
    EXPECT_CALL(inMessage, strip(Eq(sizeof(Proto::ResponseHeader))));
    EXPECT_CALL(outMessage, release());

    rpc->expectedResponses[responseId] = false;
    rpc->responsesOutstanding = 1;
    EXPECT_EQ(0, rpc->responseQueue.size());
    EXPECT_EQ(0, rpc->responses.size());

    rpc->handleResponse(&header, std::move(message));

    EXPECT_TRUE(rpc->expectedResponses.at(responseId));
    EXPECT_EQ(0, rpc->responsesOutstanding);
    EXPECT_EQ(1, rpc->responseQueue.size());
    EXPECT_EQ(1, rpc->responses.size());
    EXPECT_TRUE(rpc->pendingRequests.empty());

    EXPECT_CALL(inMessage, release());
}

TEST_F(RooPCImplTest, handleResponse_duplicate)
{
    Proto::ResponseId responseId(Proto::TaskId(2, 2), 2);
    Proto::ResponseHeader header;
    header.responseId = responseId;
    Homa::unique_ptr<Homa::InMessage> message(&inMessage);
    EXPECT_CALL(inMessage, strip(Eq(sizeof(Proto::ResponseHeader))));
    EXPECT_CALL(inMessage, release());

    rpc->expectedResponses[responseId] = true;
    EXPECT_EQ(0, rpc->responsesOutstanding);
    EXPECT_EQ(0, rpc->responseQueue.size());
    EXPECT_EQ(0, rpc->responses.size());

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
    EXPECT_EQ(0, rpc->responses.size());
}

size_t
cp(size_t, void* destination, std::size_t count, void* source)
{
    std::memcpy(destination, source, count);
    return count;
}

TEST_F(RooPCImplTest, handleManifest)
{
    Proto::BranchId branchId(Proto::TaskId(1, 1), 0);
    Proto::TaskId taskId(2, 2);
    Proto::ManifestHeader header;
    header.manifestCount = 1;
    Proto::Manifest manifest;
    manifest.requestId = Proto::RequestId(branchId, 0);
    manifest.taskId = taskId;
    manifest.requestCount = 0;
    manifest.responseCount = 0;
    Homa::unique_ptr<Homa::InMessage> message(&inMessage);

    rpc->branches[branchId] = {false, {}, {}, 0};
    rpc->manifestsOutstanding = 1;
    rpc->responsesOutstanding = 0;
    rpc->pendingRequests.push_back(
        std::move(Homa::unique_ptr<Homa::OutMessage>(&outMessage)));

    EXPECT_CALL(inMessage, get(Eq(sizeof(Proto::ManifestHeader)), _,
                               Eq(sizeof(Proto::Manifest))))
        .WillOnce(
            Invoke(std::bind(cp, std::placeholders::_1, std::placeholders::_2,
                             std::placeholders::_3, &manifest)));
    EXPECT_CALL(transport, getDriver());
    EXPECT_CALL(driver,
                getAddress(An<const Homa::Driver::WireFormatAddress*>()))
        .WillOnce(Return(0xFEED));
    EXPECT_CALL(inMessage, release());
    EXPECT_CALL(outMessage, release());

    rpc->handleManifest(&header, std::move(message));

    EXPECT_EQ(1, rpc->branches.size());
    EXPECT_EQ(0, rpc->expectedResponses.size());
    EXPECT_EQ(0, rpc->manifestsOutstanding);
    EXPECT_EQ(0, rpc->responsesOutstanding);
    EXPECT_TRUE(rpc->pendingRequests.empty());
}

ACTION_P(SaveBlob, pointer)
{
    std::memcpy(pointer, arg0, arg1);
}

TEST_F(RooPCImplTest, handlePong_basic)
{
    Proto::BranchId branchId(Proto::TaskId(1, 1), 1);
    Proto::RequestId requestId(branchId, 0);
    Proto::TaskId taskId(2, 2);
    Proto::RequestId delegatedId({{taskId, 0}, 0});
    Proto::PongHeader header;
    header.requestId = requestId;
    header.taskId = taskId;
    header.requestCount = 1;
    header.responseCount = 2;
    header.taskComplete = true;
    header.branchComplete = true;
    Homa::unique_ptr<Homa::InMessage> message(&inMessage);

    rpc->branches[branchId] = {false, {}, {}, 1};
    rpc->manifestsOutstanding = 1;

    rpc->expectedResponses[Proto::ResponseId(taskId, 0)] = true;
    rpc->responsesOutstanding = 0;

    EXPECT_EQ(0, rpc->branches.count(delegatedId.branchId));
    EXPECT_EQ(1, rpc->expectedResponses.size());

    Proto::PingHeader pingHeader;

    InSequence s;

    // Expect Start
    EXPECT_CALL(inMessage, strip(Eq(sizeof(Proto::PongHeader))));

    // Expect: Update child branches
    EXPECT_CALL(inMessage,
                get(Eq(0), _, Eq(sizeof(Homa::Driver::WireFormatAddress))));
    EXPECT_CALL(inMessage, strip(Eq(sizeof(Homa::Driver::WireFormatAddress))));
    EXPECT_CALL(transport, getDriver());
    EXPECT_CALL(driver,
                getAddress(An<const Homa::Driver::WireFormatAddress*>()))
        .WillOnce(Return(0xFEED));
    EXPECT_CALL(transport, alloc())
        .WillOnce(
            Return(ByMove(Homa::unique_ptr<Homa::OutMessage>(&outMessage))));
    EXPECT_CALL(outMessage, append(_, Eq(sizeof(Proto::PingHeader))))
        .WillOnce(SaveBlob(&pingHeader));
    EXPECT_CALL(outMessage,
                send(Eq(0xFEED), Eq(Homa::OutMessage::NO_RETRY |
                                    Homa::OutMessage::NO_KEEP_ALIVE)));
    EXPECT_CALL(outMessage, release());

    EXPECT_CALL(inMessage, release());

    // TEST CALL
    rpc->handlePong(&header, std::move(message));

    // Check branch
    EXPECT_TRUE(rpc->branches.at(branchId).complete);
    EXPECT_EQ(0, rpc->branches.at(branchId).pingTimeouts);

    // Check child request branches
    EXPECT_FALSE(rpc->branches.at(delegatedId.branchId).complete);
    EXPECT_EQ(delegatedId,
              rpc->branches.at(delegatedId.branchId).pingReceiverId);
    EXPECT_EQ(0xFEED, rpc->branches.at(delegatedId.branchId).pingAddress);
    EXPECT_EQ(delegatedId, pingHeader.requestId);
    EXPECT_EQ(2, rpc->branches.size());
    EXPECT_EQ(1, rpc->manifestsOutstanding);

    // Check responses
    EXPECT_EQ(2, rpc->expectedResponses.size());
    EXPECT_TRUE(rpc->expectedResponses.at(Proto::ResponseId(taskId, 0)));
    EXPECT_FALSE(rpc->expectedResponses.at(Proto::ResponseId(taskId, 1)));
}

TEST_F(RooPCImplTest, handlePong_unexpected)
{
    Proto::BranchId branchId(Proto::TaskId(1, 1), 1);
    Proto::RequestId requestId(branchId, 0);
    Proto::TaskId taskId(2, 2);
    Proto::PongHeader header;
    header.requestId = requestId;
    header.taskId = taskId;
    header.requestCount = 1;
    header.responseCount = 2;
    header.taskComplete = true;
    header.branchComplete = true;
    Homa::unique_ptr<Homa::InMessage> message(&inMessage);

    VectorHandler handler;
    Debug::setLogHandler(std::ref(handler));

    // Expect Start
    EXPECT_CALL(inMessage, strip(Eq(sizeof(Proto::PongHeader))));

    EXPECT_CALL(inMessage, release());

    // TEST CALL
    rpc->handlePong(&header, std::move(message));

    EXPECT_EQ(1U, handler.messages.size());
    const Debug::DebugMessage& m = handler.messages.at(0);
    EXPECT_STREQ("src/RooPCImpl.cc", m.filename);
    EXPECT_STREQ("handlePong", m.function);
    EXPECT_EQ(int(Debug::LogLevel::WARNING), m.logLevel);
    EXPECT_EQ("Unexpected PONG from RequestId {{{1, 1}, 1}, 0}; PONG dropped.",
              m.message);
    Debug::setLogHandler(std::function<void(Debug::DebugMessage)>());
}

TEST_F(RooPCImplTest, handlePong_single_delegate)
{
    Proto::BranchId branchId(Proto::TaskId(1, 1), 1);
    Proto::RequestId requestId(branchId, 0);
    Proto::TaskId taskId(2, 2);
    Proto::RequestId delegatedId(branchId, 1);
    Proto::PongHeader header;
    header.requestId = requestId;
    header.taskId = taskId;
    header.requestCount = 1;
    header.responseCount = 0;
    header.taskComplete = true;
    header.branchComplete = false;
    Homa::unique_ptr<Homa::InMessage> message(&inMessage);

    rpc->branches[branchId] = {false, requestId, {}, 0};
    rpc->manifestsOutstanding = 1;

    Proto::PingHeader pingHeader;

    InSequence s;

    // Expect Start
    EXPECT_CALL(inMessage, strip(Eq(sizeof(Proto::PongHeader))));

    // Expect: Update child branches
    EXPECT_CALL(inMessage,
                get(Eq(0), _, Eq(sizeof(Homa::Driver::WireFormatAddress))));
    EXPECT_CALL(inMessage, strip(Eq(sizeof(Homa::Driver::WireFormatAddress))));
    EXPECT_CALL(transport, getDriver());
    EXPECT_CALL(driver,
                getAddress(An<const Homa::Driver::WireFormatAddress*>()))
        .WillOnce(Return(0xFEED));
    EXPECT_CALL(transport, alloc())
        .WillOnce(
            Return(ByMove(Homa::unique_ptr<Homa::OutMessage>(&outMessage))));
    EXPECT_CALL(outMessage, append(_, Eq(sizeof(Proto::PingHeader))))
        .WillOnce(SaveBlob(&pingHeader));
    EXPECT_CALL(outMessage,
                send(Eq(0xFEED), Eq(Homa::OutMessage::NO_RETRY |
                                    Homa::OutMessage::NO_KEEP_ALIVE)));
    EXPECT_CALL(outMessage, release());

    EXPECT_CALL(inMessage, release());

    // TEST CALL
    rpc->handlePong(&header, std::move(message));

    // Check branch
    EXPECT_FALSE(rpc->branches.at(branchId).complete);
    EXPECT_EQ(0, rpc->branches.at(branchId).pingTimeouts);
    EXPECT_EQ(1, rpc->manifestsOutstanding);

    // Check delegated request
    EXPECT_FALSE(rpc->branches.at(branchId).complete);
    EXPECT_EQ(delegatedId, rpc->branches.at(branchId).pingReceiverId);
    EXPECT_EQ(0xFEED, rpc->branches.at(branchId).pingAddress);
    EXPECT_EQ(delegatedId, pingHeader.requestId);
}

TEST_F(RooPCImplTest, handleError)
{
    Proto::ErrorHeader header;
    Homa::unique_ptr<Homa::InMessage> message(&inMessage);

    EXPECT_CALL(inMessage, release());
    EXPECT_FALSE(rpc->error);

    rpc->handleError(&header, std::move(message));

    EXPECT_TRUE(rpc->error);
}

TEST_F(RooPCImplTest, handleTimeout)
{
    Proto::RooId rooId(0, 0);
    Proto::BranchId rootId(rooId, 0);
    Proto::RequestId requestId(rootId, 0);
    rpc->branches.insert({requestId.branchId, {false, requestId, 0xFEED, 3}});
    rpc->branches.insert({{rooId, 1}, {true, {}, {}, {}}});
    rpc->manifestsOutstanding = 1;

    EXPECT_FALSE(rpc->error);
    EXPECT_FALSE(rpc->branches.at(rootId).complete);
    EXPECT_EQ(3, rpc->branches.at(rootId).pingTimeouts);

    // Expect ping
    EXPECT_CALL(transport, alloc())
        .WillOnce(
            Return(ByMove(Homa::unique_ptr<Homa::OutMessage>(&outMessage))));
    EXPECT_CALL(outMessage, append(_, Eq(sizeof(Proto::PingHeader))));
    EXPECT_CALL(outMessage,
                send(Eq(0xFEED), Eq(Homa::OutMessage::NO_RETRY |
                                    Homa::OutMessage::NO_KEEP_ALIVE)));
    EXPECT_CALL(outMessage, release());
    EXPECT_TRUE(rpc->handleTimeout());

    EXPECT_FALSE(rpc->error);
    EXPECT_FALSE(rpc->branches.at(rootId).complete);
    EXPECT_EQ(4, rpc->branches.at(rootId).pingTimeouts);

    // Expect timeout
    EXPECT_FALSE(rpc->handleTimeout());

    EXPECT_TRUE(rpc->error);
    EXPECT_FALSE(rpc->branches.at(rootId).complete);
    EXPECT_EQ(4, rpc->branches.at(rootId).pingTimeouts);

    // Missing responses
    rpc->branches.at(rootId).complete = true;
    rpc->manifestsOutstanding = 0;
    rpc->responsesOutstanding = 1;
    rpc->error = false;

    EXPECT_FALSE(rpc->handleTimeout());
    EXPECT_TRUE(rpc->error);

    // All complete
    rpc->manifestsOutstanding = 0;
    rpc->responsesOutstanding = 0;
    rpc->error = false;

    EXPECT_FALSE(rpc->handleTimeout());
    EXPECT_FALSE(rpc->error);
}

TEST_F(RooPCImplTest, BranchInfo_updatePingTarget)
{
    Proto::BranchId branchId{{2, 2}, 1};
    Proto::RequestId requestId{branchId, 0};
    Proto::RequestId delegateId{branchId, 1};
    Proto::RequestId parentRequestId{{{1, 1}, 0}, 0};
    Homa::Driver::Address address = 0xFEED;
    Homa::Driver::Address delegateAddress = 0xBEEF;
    Homa::Driver::Address parentAddress = 0xDEAD;
    RooPCImpl::BranchInfo info;
    info.pingReceiverId = parentRequestId;
    info.pingAddress = parentAddress;

    bool ret = false;

    // Update from parent address
    ret = info.updatePingTarget(branchId, requestId, address);

    EXPECT_TRUE(ret);
    EXPECT_EQ(requestId, info.pingReceiverId);
    EXPECT_EQ(address, info.pingAddress);

    // Update to delegate
    ret = info.updatePingTarget(branchId, delegateId, delegateAddress);

    EXPECT_TRUE(ret);
    EXPECT_EQ(delegateId, info.pingReceiverId);
    EXPECT_EQ(delegateAddress, info.pingAddress);

    // Don't update back
    ret = info.updatePingTarget(branchId, parentRequestId, parentAddress);

    EXPECT_FALSE(ret);
    EXPECT_EQ(delegateId, info.pingReceiverId);
    EXPECT_EQ(delegateAddress, info.pingAddress);
}

TEST_F(RooPCImplTest, processManifest)
{
    Proto::BranchId branchId(Proto::TaskId(1, 1), 1);
    Proto::RequestId requestId(branchId, 42);
    Proto::TaskId taskId(2, 2);
    Proto::Manifest manifest;
    manifest.requestId = requestId;
    manifest.taskId = taskId;
    manifest.requestCount = 2;
    manifest.responseCount = 2;

    rpc->branches[Proto::BranchId(taskId, 0)] = {true, {}, {}, 0};
    rpc->expectedResponses[Proto::ResponseId(taskId, 0)] = true;

    EXPECT_EQ(1, rpc->branches.size());
    EXPECT_EQ(1, rpc->expectedResponses.size());

    EXPECT_CALL(transport, getDriver());
    EXPECT_CALL(driver,
                getAddress(TypedEq<const Homa::Driver::WireFormatAddress*>(
                    &manifest.serverAddress)))
        .WillOnce(Return(0xFEED));

    SpinLock::Lock lock(rpc->mutex);
    rpc->processManifest(&manifest, lock);

    EXPECT_EQ(1, rpc->manifestsOutstanding);
    EXPECT_EQ(1, rpc->responsesOutstanding);

    // Check tasks
    EXPECT_EQ(3, rpc->branches.size());
    EXPECT_TRUE(rpc->branches.at(Proto::BranchId(taskId, 0)).complete);

    EXPECT_FALSE(rpc->branches.at(Proto::BranchId(taskId, 1)).complete);
    EXPECT_EQ(requestId,
              rpc->branches.at(Proto::BranchId(taskId, 1)).pingReceiverId);
    EXPECT_EQ(0xFEED, rpc->branches.at(Proto::BranchId(taskId, 1)).pingAddress);
    EXPECT_EQ(0, rpc->branches.at(Proto::BranchId(taskId, 1)).pingTimeouts);

    EXPECT_TRUE(rpc->branches.at(branchId).complete);

    // Check response
    EXPECT_EQ(2, rpc->expectedResponses.size());
    EXPECT_TRUE(rpc->expectedResponses.at(Proto::ResponseId(taskId, 0)));
    EXPECT_FALSE(rpc->expectedResponses.at(Proto::ResponseId(taskId, 1)));
}

TEST_F(RooPCImplTest, updateBranchInfo)
{
    Proto::RequestId parentRequestId{{{1, 1}, 1}, 0};
    Homa::Driver::Address parentAddress = 0xFEED;
    Proto::RequestId requestId{{{2, 2}, 0}, 0};
    Homa::Driver::Address address = 0xBEEF;

    EXPECT_EQ(0, rpc->branches.size());

    SpinLock::Lock lock(rpc->mutex);

    // New, incomplete
    auto ret = rpc->updateBranchInfo(requestId.branchId, false, parentRequestId,
                                     parentAddress, lock);

    EXPECT_EQ(1, rpc->branches.size());
    EXPECT_EQ(1, rpc->manifestsOutstanding);
    EXPECT_EQ(ret.first, &rpc->branches.at(requestId.branchId));
    EXPECT_TRUE(ret.second);
    EXPECT_FALSE(ret.first->complete);
    EXPECT_EQ(parentRequestId, ret.first->pingReceiverId);
    EXPECT_EQ(parentAddress, ret.first->pingAddress);
    EXPECT_EQ(0, ret.first->pingTimeouts);

    // Tracked, updatePingTargets()
    ret = rpc->updateBranchInfo(requestId.branchId, false, requestId, address,
                                lock);

    EXPECT_EQ(1, rpc->branches.size());
    EXPECT_EQ(1, rpc->manifestsOutstanding);
    EXPECT_EQ(ret.first, &rpc->branches.at(requestId.branchId));
    EXPECT_TRUE(ret.second);
    EXPECT_FALSE(ret.first->complete);
    EXPECT_EQ(requestId, ret.first->pingReceiverId);
    EXPECT_EQ(address, ret.first->pingAddress);
    EXPECT_EQ(0, ret.first->pingTimeouts);

    // Tracked, update complete
    ret = rpc->updateBranchInfo(requestId.branchId, true, {}, {}, lock);

    EXPECT_EQ(1, rpc->branches.size());
    EXPECT_EQ(0, rpc->manifestsOutstanding);
    EXPECT_EQ(ret.first, &rpc->branches.at(requestId.branchId));
    EXPECT_TRUE(ret.second);
    EXPECT_TRUE(ret.first->complete);
    EXPECT_EQ(requestId, ret.first->pingReceiverId);
    EXPECT_EQ(address, ret.first->pingAddress);
    EXPECT_EQ(0, ret.first->pingTimeouts);

    // Tracked, already complete
    ret = rpc->updateBranchInfo(requestId.branchId, false, {}, {}, lock);

    EXPECT_EQ(1, rpc->branches.size());
    EXPECT_EQ(0, rpc->manifestsOutstanding);
    EXPECT_FALSE(ret.second);
    EXPECT_TRUE(ret.first->complete);
    EXPECT_EQ(requestId, ret.first->pingReceiverId);
    EXPECT_EQ(address, ret.first->pingAddress);
    EXPECT_EQ(0, ret.first->pingTimeouts);
}

}  // namespace
}  // namespace Roo
