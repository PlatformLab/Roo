/* Copyright (c) 2018-2020, Stanford University
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

/**
 * @file Proto.h
 *
 * This file contains wire protocol definitions for RooPC messages.
 */

#ifndef ROO_PROTO_H
#define ROO_PROTO_H

#include <Homa/Driver.h>

#include <cstdint>
#include <functional>

namespace Roo {
namespace Proto {

/**
 * A unique identifier for a Task.
 */
struct TaskId {
    uint64_t socketId;  ///< Unique id for socket that owns this task.
    uint64_t sequence;  ///< Sequence number for this task (unique for
                        ///< socketId, monotonically increasing).

    /// TaskId default constructor.
    TaskId()
        : socketId(0)
        , sequence(0)
    {}

    /// TaskId constructor.
    TaskId(uint64_t socketId, uint64_t sequence)
        : socketId(socketId)
        , sequence(sequence)
    {}

    /**
     * Comparison function for TaskId, for use in std::maps etc.
     */
    bool operator<(TaskId other) const
    {
        return (socketId < other.socketId) ||
               ((socketId == other.socketId) && (sequence < other.sequence));
    }

    /**
     * Equality function for TaskId, for use in std::unordered_maps etc.
     */
    bool operator==(TaskId other) const
    {
        return ((socketId == other.socketId) && (sequence == other.sequence));
    }

    /**
     * This class computes a hash of an TaskId, so that Id can be used
     * as keys in unordered_maps.
     */
    struct Hasher {
        /// Return a "hash" of the given TaskId.
        std::size_t operator()(const TaskId& taskId) const
        {
            std::size_t h1 = std::hash<uint64_t>()(taskId.socketId);
            std::size_t h2 = std::hash<uint64_t>()(taskId.sequence);
            return h1 ^ (h2 << 1);
        }
    };
} __attribute__((packed));

/**
 * A unique identifier for a RooPC.
 */
using RooId = TaskId;

/**
 * A unique identifier for a request branch.
 */
struct BranchId {
    TaskId taskId;      ///< Id of the task that initiated the request branch.
    uint32_t sequence;  ///< Uniquely identifies the branch within the task.

    /// TaskId default constructor.
    BranchId()
        : taskId(0, 0)
        , sequence(0)
    {}

    /// BranchId constructor.
    BranchId(TaskId taskId, uint32_t sequence)
        : taskId(taskId)
        , sequence(sequence)
    {}

    /**
     * Comparison function for BranchId, for use in std::maps etc.
     */
    bool operator<(BranchId other) const
    {
        return (taskId < other.taskId) ||
               ((taskId == other.taskId) && (sequence < other.sequence));
    }

    /**
     * Equality function for BranchId, for use in std::unordered_maps etc.
     */
    bool operator==(BranchId other) const
    {
        return ((taskId == other.taskId) && (sequence == other.sequence));
    }

    /**
     * This class computes a hash of an BranchId, so that Id can be used
     * as keys in unordered_maps.
     */
    struct Hasher {
        /// Return a "hash" of the given BranchId.
        std::size_t operator()(const BranchId& branchId) const
        {
            std::size_t h1 = TaskId::Hasher()(branchId.taskId);
            std::size_t h2 = std::hash<uint32_t>()(branchId.sequence);
            return h1 ^ (h2 << 1);
        }
    };
} __attribute__((packed));

/**
 * A unique identifier for a response.
 */
struct ResponseId {
    TaskId taskId;      ///< Id of the task that sent this response.
    uint32_t sequence;  ///< Uniquely identifies the response within the task.

    /// ResponseId default constructor.
    ResponseId()
        : taskId(0, 0)
        , sequence(0)
    {}

    /// ResponseId constructor.
    ResponseId(TaskId taskId, uint64_t sequence)
        : taskId(taskId)
        , sequence(sequence)
    {}

    /**
     * Comparison function for ResponseId, for use in std::maps etc.
     */
    bool operator<(ResponseId other) const
    {
        return (taskId < other.taskId) ||
               ((taskId == other.taskId) && (sequence < other.sequence));
    }

    /**
     * Equality function for ResponseId, for use in std::unordered_maps etc.
     */
    bool operator==(ResponseId other) const
    {
        return ((taskId == other.taskId) && (sequence == other.sequence));
    }

    /**
     * This class computes a hash of an ResponseId, so that Id can be used as
     * keys in unordered_maps.
     */
    struct Hasher {
        /// Return a "hash" of the given ResponseId.
        std::size_t operator()(const ResponseId& responseId) const
        {
            std::size_t h1 = TaskId::Hasher()(responseId.taskId);
            std::size_t h2 = std::hash<uint32_t>()(responseId.sequence);
            return h1 ^ (h2 << 1);
        }
    };
} __attribute__((packed));

/**
 * This is the first part of the Homa packet header and is common to all
 * versions of the protocol. The struct contains version information about the
 * protocol used in the encompassing Message. The Transport should always send
 * this prefix and can always expect it when receiving a Homa Message. The
 * prefix is separated into its own struct because the Transport may need to
 * know the protocol version before interpreting the rest of the packet.
 */
struct HeaderPrefix {
    uint8_t version;  ///< The version of the protocol being used by this
                      ///< Message.

    /// HeaderPrefix constructor.
    HeaderPrefix(uint8_t version)
        : version(version)
    {}
} __attribute__((packed));

/**
 * Distinguishes between different protocol messages types.  See the xxx
 * namespace and xxx::Header for more information.
 */
enum class Opcode : uint8_t {
    Request = 1,
    Response,
    Manifest,
    Invalid,
};

/**
 * Contains information needed for all protocol message types.
 */
struct HeaderCommon {
    HeaderPrefix prefix;  ///< Common to all versions of the protocol
    Opcode opcode;        ///< Distinguishes between different protocol messages

    /// HeaderCommon default constructor.
    HeaderCommon()
        : prefix(1)
        , opcode(Opcode::Invalid)
    {}

    /// HeaderCommon constructor.
    HeaderCommon(Opcode opcode)
        : prefix(1)
        , opcode(opcode)
    {}
} __attribute__((packed));

/**
 * Contains the wire format header definitions for sending and receiving a
 * RooPC request message.
 */
struct RequestHeader {
    HeaderCommon common;  ///< Common header information.
    RooId rooId;          ///< Id of the RooPC to which this request belongs.
    BranchId branchId;    ///< Branch id of this request.
    Homa::Driver::WireFormatAddress replyAddress;  ///< Replies to this request
                                                   ///< should be sent to this
                                                   ///< address.
    // TODO(cstlee): remove pad.
    char pad[0];  // Hack to make Request and Response headers the same length.

    /// RequestHeader default constructor.
    RequestHeader()
        : common(Opcode::Request)
        , rooId()
        , branchId()
        , replyAddress()
    {}

    /// RequestHeader constructor.
    explicit RequestHeader(RooId rooId, BranchId branchId)
        : common(Opcode::Request)
        , rooId(rooId)
        , branchId(branchId)
        , replyAddress()
    {}
} __attribute__((packed));

/**
 * Contains the wire format header definitions for sending and receiving a
 * RooPC response message.
 */
struct ResponseHeader {
    HeaderCommon common;    ///< Common header information.
    RooId rooId;            ///< Id of the RooPC to which this request belongs.
    BranchId branchId;      ///< Branch for which this message is a response.
    ResponseId responseId;  ///< Id of this response.

    /// ResponseHeader default constructor.
    ResponseHeader()
        : common(Opcode::Response)
        , rooId()
        , branchId()
        , responseId()
    {}

    /// ResponseHeader constructor.
    explicit ResponseHeader(RooId rooId, BranchId branchId,
                            ResponseId responseId)
        : common(Opcode::Response)
        , rooId(rooId)
        , branchId(branchId)
        , responseId(responseId)
    {}
} __attribute__((packed));

/**
 * Describes the wire format for a task Manifest message.
 */
struct Manifest {
    HeaderCommon common;  ///< Common header information.
    RooId rooId;          ///< Id of the RooPC to which this message belongs.
    BranchId branchId;    ///< The request branch that spawned the task with
                          ///< which this manifest is associated.
    TaskId taskId;        ///< The task with which this manifest is associated.
    uint32_t requestCount;   ///< Number of requests generated by this task.
    uint32_t responseCount;  ///< Number of responses sent by this task.

    /// Header default constructor.
    Manifest()
        : common(Opcode::Manifest)
        , rooId()
        , branchId()
        , taskId()
        , requestCount()
        , responseCount()
    {}

    /// Header constructor.
    explicit Manifest(RooId rooId, BranchId branchId, TaskId taskId,
                      uint64_t requestCount, uint64_t responseCount)
        : common(Opcode::Manifest)
        , rooId(rooId)
        , branchId(branchId)
        , taskId(taskId)
        , requestCount(requestCount)
        , responseCount(responseCount)
    {}
} __attribute__((packed));

}  // namespace Proto
}  // namespace Roo

#endif  // ROO_PROTO_H
