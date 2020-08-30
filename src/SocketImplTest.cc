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

#include <PerfUtils/Cycles.h>
#include <Roo/Debug.h>
#include <gtest/gtest.h>

#include <cstring>
#include <functional>

#include "Mock/MockHoma.h"
#include "RooPCImpl.h"
#include "ServerTaskImpl.h"
#include "SocketImpl.h"

namespace Roo {
namespace {

using ::testing::_;
using ::testing::An;
using ::testing::ByMove;
using ::testing::Eq;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::SetArgPointee;

class SocketImplTest : public ::testing::Test {
  public:
    SocketImplTest()
        : transport()
        , driver()
        , socket(nullptr)
        , mockIncomingRequest()
    {
        ON_CALL(transport, getDriver()).WillByDefault(Return(&driver));
        EXPECT_CALL(transport, getId()).WillOnce(Return(42));
        socket = new SocketImpl(&transport);
    }
    ~SocketImplTest()
    {
        delete socket;
    }

    SocketImpl::ServerTaskHandle* createTask(Proto::RooId rooId,
                                             Proto::RequestId requestId,
                                             Homa::Driver::Address replyAddress)
    {
        EXPECT_CALL(transport, getDriver());
        EXPECT_CALL(driver,
                    getAddress(An<const Homa::Driver::WireFormatAddress*>()))
            .WillOnce(Return(replyAddress));
        EXPECT_CALL(mockIncomingRequest, strip(An<size_t>()));
        Proto::RequestHeader header;
        header.rooId = rooId;
        header.requestId = requestId;
        Homa::unique_ptr<Homa::InMessage> request(&mockIncomingRequest);

        SocketImpl::ServerTaskHandle* handle = socket->taskPool.construct(
            socket, socket->allocTaskId(), &header, std::move(request));
        socket->tasks.insert({requestId, handle});
        socket->taskTimeouts.setTimeout(&handle->timeout);
        return handle;
    }

    Mock::Homa::MockTransport transport;
    Mock::Homa::MockDriver driver;
    SocketImpl* socket;
    NiceMock<Mock::Homa::MockInMessage> mockIncomingRequest;
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

TEST_F(SocketImplTest, constructor)
{
    EXPECT_CALL(transport, getId()).WillOnce(Return(42));
    SocketImpl socket(&transport);
    EXPECT_EQ(&transport, socket.transport);
    EXPECT_EQ(42, socket.socketId);
}

TEST_F(SocketImplTest, allocRooPC)
{
    EXPECT_EQ(1U, socket->nextSequenceNumber.load());
    EXPECT_TRUE(socket->rpcs.empty());
    Proto::RooId rooId(socket->socketId, 1U);
    Roo::unique_ptr<RooPC> rpc = socket->allocRooPC();
    EXPECT_EQ(2U, socket->nextSequenceNumber.load());
    EXPECT_FALSE(socket->rpcs.find(rooId) == socket->rpcs.end());
    EXPECT_EQ(rooId, socket->rpcTimeouts.front()->object->rpc.getId());
}

TEST_F(SocketImplTest, receive)
{
    Proto::RooId rooId(1, 1);
    Proto::RequestId requestId({{2, 2}, 2}, 0);
    SocketImpl::ServerTaskHandle* handle =
        createTask(rooId, requestId, 0xDEADBEEF);
    socket->pendingTasks.push_back(&handle->task);
    ServerTask* expected_task = &handle->task;

    {
        Roo::unique_ptr<ServerTask> task = socket->receive();
        EXPECT_EQ(expected_task, task.get());
        EXPECT_TRUE(socket->pendingTasks.empty());
        task.release();
    }

    {
        Roo::unique_ptr<ServerTask> task = socket->receive();
        EXPECT_FALSE(task);
    }
}

TEST_F(SocketImplTest, poll)
{
    // Nothing to test directly; tested as part of:
    //      processIncomingMessages()
    //      checkDetachedTasks()
    //      checkClientTimeouts()
    //      checkTaskTimeouts()
}

TEST_F(SocketImplTest, dropRooPC)
{
    Proto::RooId rooId = socket->allocTaskId();
    SocketImpl::RpcHandle* handle = socket->rpcPool.construct(socket, rooId);
    socket->rpcs.insert({rooId, handle});

    socket->dropRooPC(&handle->rpc);

    EXPECT_EQ(0, socket->rpcs.count(rooId));
    EXPECT_EQ(0, socket->rpcPool.outstandingObjects);
}

ACTION_P(FakeGet, pointer)
{
    std::memcpy(arg1, pointer, arg2);
    return arg2;
}

TEST_F(SocketImplTest, processIncomingMessages_Request)
{
    Proto::RequestHeader header;
    EXPECT_CALL(transport, receive())
        .WillOnce(Return(
            ByMove(Homa::unique_ptr<Homa::InMessage>(&mockIncomingRequest))))
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>())));
    EXPECT_CALL(mockIncomingRequest, get(0, _, Eq(sizeof(Proto::HeaderCommon))))
        .WillOnce(FakeGet(&header.common));
    EXPECT_CALL(mockIncomingRequest,
                get(0, _, Eq(sizeof(Proto::RequestHeader))))
        .WillOnce(FakeGet(&header));
    EXPECT_CALL(mockIncomingRequest, length());
    // ServerTaskImpl construction expected calls
    EXPECT_CALL(transport, getDriver());
    EXPECT_CALL(driver,
                getAddress(An<const Homa::Driver::WireFormatAddress*>()));
    EXPECT_CALL(mockIncomingRequest, strip(An<size_t>()));

    EXPECT_EQ(0, socket->pendingTasks.size());
    EXPECT_EQ(0, socket->taskPool.outstandingObjects);
    EXPECT_EQ(0, socket->taskTimeouts.list.size());

    socket->processIncomingMessages();

    EXPECT_EQ(1, socket->pendingTasks.size());
    EXPECT_EQ(1, socket->taskPool.outstandingObjects);
    EXPECT_EQ(1, socket->taskTimeouts.list.size());
}

TEST_F(SocketImplTest, processIncomingMessages_Response)
{
    Proto::RooId rooId = socket->allocTaskId();
    SocketImpl::RpcHandle* handle = socket->rpcPool.construct(socket, rooId);
    socket->rpcs.insert({rooId, handle});
    RooPCImpl* rpc = &handle->rpc;

    Mock::Homa::MockInMessage inMessage;
    Proto::ResponseHeader header;
    header.rooId = rooId;
    EXPECT_CALL(transport, receive())
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>(&inMessage))))
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>())));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::HeaderCommon))))
        .WillOnce(FakeGet(&header.common));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::ResponseHeader))))
        .WillOnce(FakeGet(&header));
    EXPECT_CALL(inMessage, length());
    // RooPCImpl::handleResponse() expected calls
    EXPECT_CALL(inMessage, strip(Eq(sizeof(Proto::ResponseHeader))));

    EXPECT_EQ(0, rpc->responseQueue.size());

    socket->processIncomingMessages();

    EXPECT_EQ(1, rpc->responseQueue.size());

    EXPECT_CALL(inMessage, release());
    socket->dropRooPC(rpc);
}

TEST_F(SocketImplTest, processIncomingMessages_Manifest)
{
    Proto::RooId rooId = socket->allocTaskId();
    SocketImpl::RpcHandle* handle = socket->rpcPool.construct(socket, rooId);
    socket->rpcs.insert({rooId, handle});
    RooPCImpl* rpc = &handle->rpc;

    Mock::Homa::MockInMessage inMessage;
    Proto::RequestId requestId({{2, 2}, 2}, 0);
    Proto::ManifestHeader header;
    header.rooId = rooId;
    header.manifestCount = 1;
    Proto::Manifest manifest;
    manifest.requestId = requestId;
    EXPECT_CALL(transport, receive())
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>(&inMessage))))
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>())));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::HeaderCommon))))
        .WillOnce(FakeGet(&header.common));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::ManifestHeader))))
        .WillOnce(FakeGet(&header));
    // RooPCImpl::handleManifest() expected calls
    EXPECT_CALL(inMessage, get(Eq(sizeof(Proto::ManifestHeader)), _,
                               Eq(sizeof(Proto::Manifest))))
        .WillOnce(FakeGet(&manifest));
    EXPECT_CALL(transport, getDriver());
    EXPECT_CALL(driver,
                getAddress(An<const Homa::Driver::WireFormatAddress*>()));
    EXPECT_CALL(inMessage, release());

    EXPECT_EQ(0, rpc->branches.size());

    socket->processIncomingMessages();

    EXPECT_EQ(1, rpc->branches.size());
}

TEST_F(SocketImplTest, processIncomingMessages_Ping)
{
    Proto::RequestId requestId({{2, 2}, 2}, 0);
    createTask({1, 1}, requestId, 0xFEED);

    Mock::Homa::MockInMessage inMessage;
    Mock::Homa::MockOutMessage outMessage;
    Proto::PingHeader header(requestId);
    EXPECT_CALL(transport, receive())
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>(&inMessage))))
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>())));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::HeaderCommon))))
        .WillOnce(FakeGet(&header.common));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::PingHeader))))
        .WillOnce(FakeGet(&header));
    // ServerTaskImpl::handlePing() expected calls
    EXPECT_CALL(transport, alloc())
        .WillOnce(
            Return(ByMove(Homa::unique_ptr<Homa::OutMessage>(&outMessage))));
    EXPECT_CALL(outMessage, append(_, Eq(sizeof(Proto::PongHeader))));
    EXPECT_CALL(outMessage, append(_, Eq(0)));
    EXPECT_CALL(outMessage, send(_, _));
    EXPECT_CALL(outMessage, release());
    EXPECT_CALL(inMessage, release());

    socket->processIncomingMessages();
}

TEST_F(SocketImplTest, processIncomingMessages_Pong)
{
    Proto::RooId rooId = socket->allocTaskId();
    SocketImpl::RpcHandle* handle = socket->rpcPool.construct(socket, rooId);
    socket->rpcs.insert({rooId, handle});
    RooPCImpl* rpc = &handle->rpc;

    Mock::Homa::MockInMessage inMessage;
    Proto::RequestId requestId({{2, 2}, 2}, 0);
    Proto::PongHeader header;
    header.rooId = rooId;
    header.branchComplete = false;
    header.requestId = requestId;
    EXPECT_CALL(transport, receive())
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>(&inMessage))))
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>())));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::HeaderCommon))))
        .WillOnce(FakeGet(&header.common));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::PongHeader))))
        .WillOnce(FakeGet(&header));
    // RooPCImpl::handlePong() expected calls
    EXPECT_CALL(inMessage, strip(Eq(sizeof(Proto::PongHeader))));
    EXPECT_CALL(inMessage, release());

    VectorHandler handler;
    Debug::setLogHandler(std::ref(handler));
    socket->processIncomingMessages();

    EXPECT_EQ(1U, handler.messages.size());
    const Debug::DebugMessage& m = handler.messages.at(0);
    EXPECT_STREQ("handlePong", m.function);
    Debug::setLogHandler(std::function<void(Debug::DebugMessage)>());
}

TEST_F(SocketImplTest, processIncomingMessages_Error)
{
    Proto::RooId rooId = socket->allocTaskId();
    SocketImpl::RpcHandle* handle = socket->rpcPool.construct(socket, rooId);
    socket->rpcs.insert({rooId, handle});
    RooPCImpl* rpc = &handle->rpc;

    Mock::Homa::MockInMessage inMessage;
    Proto::ErrorHeader header;
    header.rooId = rooId;
    EXPECT_CALL(transport, receive())
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>(&inMessage))))
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>())));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::HeaderCommon))))
        .WillOnce(FakeGet(&header.common));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::ErrorHeader))))
        .WillOnce(FakeGet(&header));
    // RooPCImpl::handlePong() expected calls
    EXPECT_CALL(inMessage, release());

    EXPECT_FALSE(rpc->error);

    socket->processIncomingMessages();

    EXPECT_TRUE(rpc->error);
}

TEST_F(SocketImplTest, processIncomingMessages_invalid)
{
    Mock::Homa::MockInMessage inMessage;
    Proto::RequestHeader header;
    header.common.opcode = Proto::Opcode::Invalid;
    EXPECT_CALL(transport, receive())
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>(&inMessage))))
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>())));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::HeaderCommon))))
        .WillOnce(FakeGet(&header.common));

    EXPECT_CALL(inMessage, release());

    VectorHandler handler;
    Debug::setLogHandler(std::ref(handler));

    socket->processIncomingMessages();

    EXPECT_EQ(1U, handler.messages.size());
    const Debug::DebugMessage& m = handler.messages.at(0);
    EXPECT_STREQ("src/SocketImpl.cc", m.filename);
    EXPECT_STREQ("processIncomingMessages", m.function);
    EXPECT_EQ(int(Debug::LogLevel::WARNING), m.logLevel);
    EXPECT_EQ("Unexpected protocol message received.", m.message);
    Debug::setLogHandler(std::function<void(Debug::DebugMessage)>());
}

TEST_F(SocketImplTest, checkClientTimeouts)
{
    Proto::RooId rooId[3];
    SocketImpl::RpcHandle* handle[3];
    for (int i = 0; i < 3; ++i) {
        rooId[i] = socket->allocTaskId();
        handle[i] = socket->rpcPool.construct(socket, rooId[i]);
        socket->rpcs.insert({rooId[i], handle[i]});
    }

    uint64_t now = 100 * socket->rpcTimeouts.timeoutIntervalCycles;
    uint64_t past = now / 2;
    uint64_t future = now * 2;

    // [0] Expired, Reschedule.
    handle[0]->rpc.manifestsOutstanding = 1;
    PerfUtils::Cycles::mockTscValue = past;
    socket->rpcTimeouts.setTimeout(&handle[0]->timeout);

    // [1] Expired, No reschedule.
    handle[1]->rpc.branches.insert({{}, {false, {}, {}, 9001}});
    handle[1]->rpc.manifestsOutstanding = 1;
    PerfUtils::Cycles::mockTscValue = past;
    socket->rpcTimeouts.setTimeout(&handle[1]->timeout);

    // [2] Not expired.
    PerfUtils::Cycles::mockTscValue = future;
    socket->rpcTimeouts.setTimeout(&handle[2]->timeout);

    EXPECT_EQ(3, socket->rpcTimeouts.list.size());

    PerfUtils::Cycles::mockTscValue = now;
    socket->checkClientTimeouts();

    EXPECT_EQ(2, socket->rpcTimeouts.list.size());
    EXPECT_EQ(handle[2], socket->rpcTimeouts.list.front().object);
    EXPECT_EQ(handle[0], socket->rpcTimeouts.list.back().object);
}

TEST_F(SocketImplTest, checkTaskTimeouts)
{
    Mock::Homa::MockInMessage request;
    Proto::RequestId requestId[3];
    SocketImpl::ServerTaskHandle* handle[3];

    for (uint i = 0; i < 3; ++i) {
        requestId[i] = {{{2, 2}, i}, 0};
        handle[i] = createTask(requestId[i].branchId.taskId, requestId[i], {});
        handle[i]->task.detached = true;
    }

    uint64_t now = 100 * socket->taskTimeouts.timeoutIntervalCycles;
    uint64_t past = now / 2;
    uint64_t future = now * 2;

    // [0] Expired, reschedule.
    handle[0]->task.pingInfo.pingCount = 9001;
    PerfUtils::Cycles::mockTscValue = past;
    socket->taskTimeouts.setTimeout(&handle[0]->timeout);

    // [1] Expired, done.
    PerfUtils::Cycles::mockTscValue = past;
    socket->taskTimeouts.setTimeout(&handle[1]->timeout);

    // [2] Not expired.
    PerfUtils::Cycles::mockTscValue = future;
    socket->taskTimeouts.setTimeout(&handle[2]->timeout);

    EXPECT_EQ(3, socket->taskTimeouts.list.size());
    EXPECT_EQ(3, socket->tasks.size());

    PerfUtils::Cycles::mockTscValue = now;

    EXPECT_CALL(mockIncomingRequest, release()).Times(1);
    socket->checkTaskTimeouts();

    EXPECT_EQ(2, socket->taskTimeouts.list.size());
    EXPECT_EQ(handle[2], socket->taskTimeouts.list.front().object);
    EXPECT_EQ(handle[0], socket->taskTimeouts.list.back().object);
    EXPECT_EQ(2, socket->tasks.size());
    EXPECT_EQ(0, socket->tasks.count(requestId[1]));

    ::testing::Mock::VerifyAndClearExpectations(&mockIncomingRequest);
}

TEST_F(SocketImplTest, allocTaskId)
{
    EXPECT_EQ(Proto::TaskId(42, 1), socket->allocTaskId());
    EXPECT_EQ(Proto::TaskId(42, 2), socket->allocTaskId());
}

}  // namespace
}  // namespace Roo
