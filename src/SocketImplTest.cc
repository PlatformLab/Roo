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
using ::testing::Return;
using ::testing::SetArgPointee;

class SocketImplTest : public ::testing::Test {
  public:
    SocketImplTest()
        : transport()
        , driver()
        , socket(nullptr)
    {
        ON_CALL(transport, getDriver()).WillByDefault(Return(&driver));
        EXPECT_CALL(transport, getId()).WillOnce(Return(42));
        socket = new SocketImpl(&transport);
    }
    ~SocketImplTest()
    {
        delete socket;
    }

    ServerTaskImpl* createTask(Proto::RooId rooId, Proto::RequestId requestId,
                               Homa::Driver::Address replyAddress,
                               Mock::Homa::MockInMessage& inMessage)
    {
        EXPECT_CALL(transport, getDriver());
        EXPECT_CALL(driver,
                    getAddress(An<const Homa::Driver::WireFormatAddress*>()))
            .WillOnce(Return(replyAddress));
        EXPECT_CALL(inMessage, strip(An<size_t>()));
        Proto::RequestHeader header;
        header.rooId = rooId;
        header.requestId = requestId;
        Homa::unique_ptr<Homa::InMessage> request(&inMessage);
        return new ServerTaskImpl(socket, Proto::TaskId(42, 1), &header,
                                  std::move(request));
    }

    Mock::Homa::MockTransport transport;
    Mock::Homa::MockDriver driver;
    SocketImpl* socket;
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
    EXPECT_EQ(rooId, socket->rpcTimeouts.front()->object);
}

TEST_F(SocketImplTest, receive)
{
    Mock::Homa::MockInMessage inMessage;
    Proto::RooId rooId(1, 1);
    Proto::RequestId requestId({{2, 2}, 2}, 0);
    ServerTaskImpl* newtask =
        createTask(rooId, requestId, 0xDEADBEEF, inMessage);
    socket->pendingTasks.push_back(newtask);
    ServerTask* expected_task = newtask;

    {
        Roo::unique_ptr<ServerTask> task = socket->receive();
        EXPECT_EQ(expected_task, task.get());
        EXPECT_TRUE(socket->pendingTasks.empty());
        delete task.release();
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
    RooPCImpl* rpc = new RooPCImpl(socket, rooId);
    socket->rpcs.insert({rooId, rpc});

    socket->dropRooPC(rpc);

    EXPECT_EQ(0, socket->rpcs.count(rooId));
}

TEST_F(SocketImplTest, remandTask)
{
    Mock::Homa::MockInMessage inMessage;
    Proto::RooId rooId(1, 1);
    Proto::RequestId requestId({{2, 2}, 2}, 0);
    ServerTaskImpl* task = createTask(rooId, requestId, 0xDEADBEEF, inMessage);

    EXPECT_EQ(0, socket->detachedTasks.size());

    socket->remandTask(task);

    EXPECT_EQ(1, socket->detachedTasks.size());

    EXPECT_CALL(inMessage, release());
    delete task;
}

ACTION_P(FakeGet, pointer)
{
    std::memcpy(arg1, pointer, arg2);
    return arg2;
}

TEST_F(SocketImplTest, processIncomingMessages_Request)
{
    Mock::Homa::MockInMessage inMessage;
    Proto::RequestHeader header;
    EXPECT_CALL(transport, receive())
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>(&inMessage))))
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>())));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::HeaderCommon))))
        .WillOnce(FakeGet(&header.common));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::RequestHeader))))
        .WillOnce(FakeGet(&header));
    EXPECT_CALL(inMessage, length());
    // ServerTaskImpl construction expected calls
    EXPECT_CALL(transport, getDriver());
    EXPECT_CALL(driver,
                getAddress(An<const Homa::Driver::WireFormatAddress*>()));
    EXPECT_CALL(inMessage, strip(An<size_t>()));

    EXPECT_EQ(0, socket->pendingTasks.size());

    socket->processIncomingMessages();

    EXPECT_EQ(1, socket->pendingTasks.size());
}

TEST_F(SocketImplTest, processIncomingMessages_Response)
{
    Proto::RooId rooId = socket->allocTaskId();
    RooPCImpl* rpc = new RooPCImpl(socket, rooId);
    socket->rpcs.insert({rooId, rpc});

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
}

TEST_F(SocketImplTest, processIncomingMessages_Manifest)
{
    Proto::RooId rooId = socket->allocTaskId();
    RooPCImpl* rpc = new RooPCImpl(socket, rooId);
    socket->rpcs.insert({rooId, rpc});

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
    Proto::RequestHeader requestHeader(requestId.branchId.taskId, requestId);
    Mock::Homa::MockInMessage request;
    // ServerTaskImpl construction expected calls
    EXPECT_CALL(transport, getDriver());
    EXPECT_CALL(driver,
                getAddress(An<const Homa::Driver::WireFormatAddress*>()));
    EXPECT_CALL(request, strip(An<size_t>()));
    ServerTaskImpl* task =
        new ServerTaskImpl(socket, socket->allocTaskId(), &requestHeader,
                           Homa::unique_ptr<Homa::InMessage>(&request));
    socket->tasks.insert({task->getRequestId(), task});
    socket->pendingTasks.push_back(task);

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
    RooPCImpl* rpc = new RooPCImpl(socket, rooId);
    socket->rpcs.insert({rooId, rpc});

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
    RooPCImpl* rpc = new RooPCImpl(socket, rooId);
    socket->rpcs.insert({rooId, rpc});

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

TEST_F(SocketImplTest, checkDetachedTasks)
{
    Mock::Homa::MockInMessage inMessage;
    Mock::Homa::MockOutMessage outMessage;
    Proto::RooId rooId(1, 1);
    for (uint32_t i = 0; i < 2; ++i) {
        Proto::RequestId requestId({{2, 2}, i}, 0);
        ServerTaskImpl* task =
            createTask(rooId, requestId, 0xDEADBEEF, inMessage);
        task->pendingMessages.push_back(
            Homa::unique_ptr<Homa::OutMessage>(&outMessage));
        socket->detachedTasks.push_back(task);
    }

    // Expected ServerTaskImpl::poll() calls
    EXPECT_CALL(inMessage, dropped())
        .WillOnce(Return(true))
        .WillOnce(Return(false));
    EXPECT_CALL(outMessage, getStatus)
        .WillOnce(Return(Homa::OutMessage::Status::IN_PROGRESS));

    EXPECT_EQ(2, socket->detachedTasks.size());
    EXPECT_EQ(0, socket->taskTimeouts.list.size());

    socket->checkDetachedTasks();

    EXPECT_EQ(1, socket->detachedTasks.size());
    EXPECT_EQ(1, socket->taskTimeouts.list.size());
}

TEST_F(SocketImplTest, checkClientTimeouts)
{
    Proto::RooId rooId[4];
    Timeout<Proto::RooId>* timeout[4];
    for (int i = 0; i < 4; ++i) {
        rooId[i] = socket->allocTaskId();
        timeout[i] = socket->rpcTimeoutPool.construct(rooId[i]);
    }
    RooPCImpl* rpc[2];
    for (int i = 0; i < 2; ++i) {
        rpc[i] = new RooPCImpl(socket, rooId[i]);
    }

    uint64_t now = 100 * socket->rpcTimeouts.timeoutIntervalCycles;
    uint64_t past = now / 2;
    uint64_t future = now * 2;

    // [0] Expired, Reschedule.
    socket->rpcs.insert({rooId[0], rpc[0]});
    rpc[0]->manifestsOutstanding = 1;
    PerfUtils::Cycles::mockTscValue = past;
    socket->rpcTimeouts.setTimeout(timeout[0]);

    // [1] Expired, No reschedule.
    socket->rpcs.insert({rooId[1], rpc[1]});
    rpc[1]->branches.insert({{}, {false, {}, {}, 9001}});
    rpc[1]->manifestsOutstanding = 1;
    PerfUtils::Cycles::mockTscValue = past;
    socket->rpcTimeouts.setTimeout(timeout[1]);

    // [2] Expired, stale.
    PerfUtils::Cycles::mockTscValue = past;
    socket->rpcTimeouts.setTimeout(timeout[2]);

    // [3] Not expired.
    PerfUtils::Cycles::mockTscValue = future;
    socket->rpcTimeouts.setTimeout(timeout[3]);

    EXPECT_EQ(4, socket->rpcTimeouts.list.size());

    PerfUtils::Cycles::mockTscValue = now;
    socket->checkClientTimeouts();

    EXPECT_EQ(2, socket->rpcTimeouts.list.size());
    EXPECT_EQ(rooId[3], socket->rpcTimeouts.list.front().object);
    EXPECT_EQ(rooId[0], socket->rpcTimeouts.list.back().object);
}

TEST_F(SocketImplTest, checkTaskTimeouts)
{
    Mock::Homa::MockInMessage request;
    Proto::RequestId requestId[3];
    ServerTaskImpl* task[3];
    Timeout<ServerTaskImpl*>* timeout[3];

    for (uint i = 0; i < 3; ++i) {
        requestId[i] = {{{2, 2}, i}, 0};
        Proto::RequestHeader requestHeader(requestId[i].branchId.taskId,
                                           requestId[i]);
        // ServerTaskImpl construction expected calls
        EXPECT_CALL(transport, getDriver());
        EXPECT_CALL(driver,
                    getAddress(An<const Homa::Driver::WireFormatAddress*>()));
        EXPECT_CALL(request, strip(An<size_t>()));
        task[i] =
            new ServerTaskImpl(socket, socket->allocTaskId(), &requestHeader,
                               Homa::unique_ptr<Homa::InMessage>(&request));
        socket->tasks.insert({task[i]->getRequestId(), task[i]});
        timeout[i] = socket->taskTimeoutPool.construct(task[i]);
    }

    uint64_t now = 100 * socket->taskTimeouts.timeoutIntervalCycles;
    uint64_t past = now / 2;
    uint64_t future = now * 2;

    // [0] Expired, reschedule.
    task[0]->pingInfo.pingCount = 9001;
    PerfUtils::Cycles::mockTscValue = past;
    socket->taskTimeouts.setTimeout(timeout[0]);

    // [1] Expired, done.
    PerfUtils::Cycles::mockTscValue = past;
    socket->taskTimeouts.setTimeout(timeout[1]);

    // [2] Not expired.
    PerfUtils::Cycles::mockTscValue = future;
    socket->taskTimeouts.setTimeout(timeout[2]);

    EXPECT_EQ(3, socket->taskTimeouts.list.size());
    EXPECT_EQ(3, socket->tasks.size());

    PerfUtils::Cycles::mockTscValue = now;

    EXPECT_CALL(request, release()).Times(1);
    socket->checkTaskTimeouts();

    EXPECT_EQ(2, socket->taskTimeouts.list.size());
    EXPECT_EQ(task[2], socket->taskTimeouts.list.front().object);
    EXPECT_EQ(task[0], socket->taskTimeouts.list.back().object);
    EXPECT_EQ(2, socket->tasks.size());
    EXPECT_EQ(0, socket->tasks.count(requestId[1]));
}

TEST_F(SocketImplTest, allocTaskId)
{
    EXPECT_EQ(Proto::TaskId(42, 1), socket->allocTaskId());
    EXPECT_EQ(Proto::TaskId(42, 2), socket->allocTaskId());
}

}  // namespace
}  // namespace Roo
