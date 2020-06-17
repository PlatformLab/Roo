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

    ServerTaskImpl* createTask(Proto::RooId rooId, Proto::BranchId branchId,
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
        header.branchId = branchId;
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
}

TEST_F(SocketImplTest, receive)
{
    Mock::Homa::MockInMessage inMessage;
    Proto::RooId rooId(1, 1);
    Proto::BranchId branchId(Proto::RooId(2, 2), 2);
    ServerTaskImpl* newtask =
        createTask(rooId, branchId, 0xDEADBEEF, inMessage);
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

size_t
cp(size_t, void* destination, size_t count, void* source)
{
    std::memcpy(destination, source, count);
    return count;
}

TEST_F(SocketImplTest, poll_request)
{
    Mock::Homa::MockInMessage inMessage;
    Proto::RequestHeader header;
    EXPECT_CALL(transport, poll);
    EXPECT_CALL(transport, receive())
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>(&inMessage))))
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>())));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::HeaderCommon))))
        .WillOnce(
            Invoke(std::bind(cp, std::placeholders::_1, std::placeholders::_2,
                             std::placeholders::_3, &header.common)));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::RequestHeader))))
        .WillOnce(
            Invoke(std::bind(cp, std::placeholders::_1, std::placeholders::_2,
                             std::placeholders::_3, &header)));
    EXPECT_CALL(inMessage, length());
    // ServerTaskImpl construction expected calls
    EXPECT_CALL(transport, getDriver());
    EXPECT_CALL(driver,
                getAddress(An<const Homa::Driver::WireFormatAddress*>()));
    EXPECT_CALL(inMessage, strip(An<size_t>()));

    EXPECT_EQ(0, socket->pendingTasks.size());

    socket->poll();

    EXPECT_EQ(1, socket->pendingTasks.size());
}

TEST_F(SocketImplTest, poll_response)
{
    Proto::RooId rooId = socket->allocTaskId();
    RooPCImpl* rpc = new RooPCImpl(socket, rooId);
    socket->rpcs.insert({rooId, rpc});

    Mock::Homa::MockInMessage inMessage;
    Proto::ResponseHeader header;
    header.rooId = rooId;
    EXPECT_CALL(transport, poll);
    EXPECT_CALL(transport, receive())
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>(&inMessage))))
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>())));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::HeaderCommon))))
        .WillOnce(
            Invoke(std::bind(cp, std::placeholders::_1, std::placeholders::_2,
                             std::placeholders::_3, &header.common)));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::ResponseHeader))))
        .WillOnce(
            Invoke(std::bind(cp, std::placeholders::_1, std::placeholders::_2,
                             std::placeholders::_3, &header)));
    EXPECT_CALL(inMessage, length());
    // RooPCImpl::handleResponse() expected calls
    EXPECT_CALL(inMessage, acknowledge());
    EXPECT_CALL(inMessage, strip(Eq(sizeof(Proto::ResponseHeader))));

    EXPECT_EQ(0, rpc->responseQueue.size());

    socket->poll();

    EXPECT_EQ(1, rpc->responseQueue.size());
}

TEST_F(SocketImplTest, poll_response_stale)
{
    Proto::RooId rooId = socket->allocTaskId();

    Mock::Homa::MockInMessage inMessage;
    Proto::ResponseHeader header;
    header.rooId = rooId;
    EXPECT_CALL(transport, poll);
    EXPECT_CALL(transport, receive())
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>(&inMessage))))
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>())));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::HeaderCommon))))
        .WillOnce(
            Invoke(std::bind(cp, std::placeholders::_1, std::placeholders::_2,
                             std::placeholders::_3, &header.common)));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::ResponseHeader))))
        .WillOnce(
            Invoke(std::bind(cp, std::placeholders::_1, std::placeholders::_2,
                             std::placeholders::_3, &header)));
    EXPECT_CALL(inMessage, length());
    EXPECT_CALL(inMessage, release());

    socket->poll();
}

TEST_F(SocketImplTest, poll_manifest)
{
    Proto::RooId rooId = socket->allocTaskId();
    RooPCImpl* rpc = new RooPCImpl(socket, rooId);
    socket->rpcs.insert({rooId, rpc});

    Mock::Homa::MockInMessage inMessage;
    Proto::Manifest header;
    header.rooId = rooId;
    header.branchId = Proto::BranchId(Proto::TaskId(2, 2), 2);
    EXPECT_CALL(transport, poll);
    EXPECT_CALL(transport, receive())
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>(&inMessage))))
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>())));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::HeaderCommon))))
        .WillOnce(
            Invoke(std::bind(cp, std::placeholders::_1, std::placeholders::_2,
                             std::placeholders::_3, &header.common)));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::Manifest))))
        .WillOnce(
            Invoke(std::bind(cp, std::placeholders::_1, std::placeholders::_2,
                             std::placeholders::_3, &header)));
    // RooPCImpl::handleManifest() expected calls
    EXPECT_CALL(inMessage, acknowledge());
    EXPECT_CALL(inMessage, release());

    EXPECT_EQ(0, rpc->tasks.size());

    socket->poll();

    EXPECT_EQ(1, rpc->tasks.size());
}

TEST_F(SocketImplTest, poll_manifest_stale)
{
    Proto::RooId rooId = socket->allocTaskId();

    Mock::Homa::MockInMessage inMessage;
    Proto::Manifest header;
    header.rooId = rooId;
    header.branchId = Proto::BranchId(Proto::TaskId(2, 2), 2);
    EXPECT_CALL(transport, poll);
    EXPECT_CALL(transport, receive())
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>(&inMessage))))
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>())));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::HeaderCommon))))
        .WillOnce(
            Invoke(std::bind(cp, std::placeholders::_1, std::placeholders::_2,
                             std::placeholders::_3, &header.common)));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::Manifest))))
        .WillOnce(
            Invoke(std::bind(cp, std::placeholders::_1, std::placeholders::_2,
                             std::placeholders::_3, &header)));
    EXPECT_CALL(inMessage, release());

    socket->poll();
}

TEST_F(SocketImplTest, poll_invalid)
{
    Mock::Homa::MockInMessage inMessage;
    Proto::RequestHeader header;
    header.common.opcode = Proto::Opcode::Invalid;
    EXPECT_CALL(transport, poll);
    EXPECT_CALL(transport, receive())
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>(&inMessage))))
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>())));
    EXPECT_CALL(inMessage, get(0, _, Eq(sizeof(Proto::HeaderCommon))))
        .WillOnce(
            Invoke(std::bind(cp, std::placeholders::_1, std::placeholders::_2,
                             std::placeholders::_3, &header.common)));

    EXPECT_CALL(inMessage, release());

    VectorHandler handler;
    Debug::setLogHandler(std::ref(handler));

    socket->poll();

    EXPECT_EQ(1U, handler.messages.size());
    const Debug::DebugMessage& m = handler.messages.at(0);
    EXPECT_STREQ("src/SocketImpl.cc", m.filename);
    EXPECT_STREQ("poll", m.function);
    EXPECT_EQ(int(Debug::LogLevel::WARNING), m.logLevel);
    EXPECT_EQ("Unexpected protocol message received.", m.message);
    Debug::setLogHandler(std::function<void(Debug::DebugMessage)>());
}

TEST_F(SocketImplTest, poll_detached_tasks)
{
    Mock::Homa::MockInMessage inMessage;
    Mock::Homa::MockOutMessage outMessage;
    Proto::RooId rooId(1, 1);
    for (int i = 0; i < 2; ++i) {
        Proto::BranchId branchId(Proto::RooId(2, 2), i);
        ServerTaskImpl* task =
            createTask(rooId, branchId, 0xDEADBEEF, inMessage);
        task->pendingMessages.push_back(&outMessage);
        socket->detachedTasks.push_back(task);
    }

    EXPECT_CALL(transport, poll);
    EXPECT_CALL(transport, receive())
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>())));
    EXPECT_CALL(inMessage, dropped())
        .WillOnce(Return(true))
        .WillOnce(Return(false));
    EXPECT_CALL(outMessage, getStatus)
        .WillOnce(Return(Homa::OutMessage::Status::SENT));
    EXPECT_CALL(inMessage, release());

    EXPECT_EQ(2, socket->detachedTasks.size());

    socket->poll();

    EXPECT_EQ(1, socket->detachedTasks.size());

    EXPECT_CALL(transport, poll);
    EXPECT_CALL(transport, receive())
        .WillOnce(Return(ByMove(Homa::unique_ptr<Homa::InMessage>())));
    EXPECT_CALL(inMessage, dropped()).WillOnce(Return(true));
    EXPECT_CALL(inMessage, release());

    socket->poll();

    EXPECT_EQ(0, socket->detachedTasks.size());
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
    Proto::BranchId branchId(Proto::RooId(2, 2), 2);
    ServerTaskImpl* task = createTask(rooId, branchId, 0xDEADBEEF, inMessage);

    EXPECT_EQ(0, socket->detachedTasks.size());

    socket->remandTask(task);

    EXPECT_EQ(1, socket->detachedTasks.size());

    EXPECT_CALL(inMessage, release());
    delete task;
}

TEST_F(SocketImplTest, allocTaskId)
{
    EXPECT_EQ(Proto::TaskId(42, 1), socket->allocTaskId());
    EXPECT_EQ(Proto::TaskId(42, 2), socket->allocTaskId());
}

}  // namespace
}  // namespace Roo
