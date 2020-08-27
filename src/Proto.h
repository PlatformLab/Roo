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
#include <cstring>
#include <functional>
#include <string>

#include "StringUtil.h"

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
    bool operator<(const TaskId& other) const
    {
        return (socketId < other.socketId) ||
               ((socketId == other.socketId) && (sequence < other.sequence));
    }

    /**
     * Equality function for TaskId, for use in std::unordered_maps etc.
     */
    bool operator==(const TaskId& other) const
    {
        return ((socketId == other.socketId) && (sequence == other.sequence));
    }

    /**
     * Return a string representation of the TaskId.
     *
     * Useful for logging.
     */
    std::string toString() const
    {
        return StringUtil::format("{%lX, %lu}", socketId, sequence);
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

    /// BranchId default constructor.
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
    bool operator<(const BranchId& other) const
    {
        return (taskId < other.taskId) ||
               ((taskId == other.taskId) && (sequence < other.sequence));
    }

    /**
     * Equality function for BranchId, for use in std::unordered_maps etc.
     */
    bool operator==(const BranchId& other) const
    {
        return ((taskId == other.taskId) && (sequence == other.sequence));
    }

    /**
     * Return a string representation of the BranchId.
     *
     * Useful for logging.
     */
    std::string toString() const
    {
        return StringUtil::format("{%s, %u}", taskId.toString().c_str(),
                                  sequence);
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
 * A unique identifier for a request.
 */
struct RequestId {
    BranchId branchId;  ///< Id of the request branch this requst belongs to.
    uint32_t sequence;  ///< Uniquely identifies the request within the branch.

    /// RequestId default constructor.
    RequestId()
        : branchId()
        , sequence(0)
    {}

    /// RequestId constructor.
    RequestId(BranchId branchId, uint32_t sequence)
        : branchId(branchId)
        , sequence(sequence)
    {}

    /**
     * Comparison function for RequestId, for use in std::maps etc.
     */
    bool operator<(const RequestId& other) const
    {
        return (branchId < other.branchId) ||
               ((branchId == other.branchId) && (sequence < other.sequence));
    }

    /**
     * Equality function for RequestId, for use in std::unordered_maps etc.
     */
    bool operator==(const RequestId& other) const
    {
        return ((branchId == other.branchId) && (sequence == other.sequence));
    }

    /**
     * Return a string representation of the RequestId.
     *
     * Useful for logging.
     */
    std::string toString() const
    {
        return StringUtil::format("{%s, %u}", branchId.toString().c_str(),
                                  sequence);
    }

    /**
     * This class computes a hash of an RequestId, so that Id can be used
     * as keys in unordered_maps.
     */
    struct Hasher {
        /// Return a "hash" of the given RequestId.
        std::size_t operator()(const RequestId& requestId) const
        {
            std::size_t h1 = BranchId::Hasher()(requestId.branchId);
            std::size_t h2 = std::hash<uint32_t>()(requestId.sequence);
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
    ResponseId(TaskId taskId, uint32_t sequence)
        : taskId(taskId)
        , sequence(sequence)
    {}

    /**
     * Comparison function for ResponseId, for use in std::maps etc.
     */
    bool operator<(const ResponseId& other) const
    {
        return (taskId < other.taskId) ||
               ((taskId == other.taskId) && (sequence < other.sequence));
    }

    /**
     * Equality function for ResponseId, for use in std::unordered_maps etc.
     */
    bool operator==(const ResponseId& other) const
    {
        return ((taskId == other.taskId) && (sequence == other.sequence));
    }

    /**
     * Return a string representation of the ResponseId.
     *
     * Useful for logging.
     */
    std::string toString() const
    {
        return StringUtil::format("{%s, %u}", taskId.toString().c_str(),
                                  sequence);
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
 * Describes the wire format for a task Manifest.
 */
struct Manifest {
    RequestId requestId;     ///< Id of the request that spawned this task.
    TaskId taskId;           ///< Id of this task.
    uint32_t requestCount;   ///< Number of requests generated by this task.
    uint32_t responseCount;  ///< Number of responses sent by this task.
    Homa::Driver::WireFormatAddress
        serverAddress;  ///< Address of the server that processed this task.

    /// Header default constructor.
    Manifest()
        : requestId()
        , taskId()
        , requestCount()
        , responseCount()
        , serverAddress()
    {}

    /// Header constructor.
    explicit Manifest(RequestId requestId, TaskId taskId, uint64_t requestCount,
                      uint64_t responseCount)
        : requestId(requestId)
        , taskId(taskId)
        , requestCount(requestCount)
        , responseCount(responseCount)
        , serverAddress()
    {}

    /**
     * Equality function for a Manifest struct; used for unit testing.
     */
    bool operator==(const Manifest& other) const
    {
        return (requestId == other.requestId) && (taskId == other.taskId) &&
               (requestCount == other.requestCount) &&
               (responseCount == other.responseCount) &&
               (0 == std::memcmp(&serverAddress, &other.serverAddress,
                                 sizeof(Homa::Driver::WireFormatAddress)));
    }
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
    Ping,
    Pong,
    Error,
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
    RequestId requestId;  ///< Id of this request.
    Homa::Driver::WireFormatAddress replyAddress;  ///< Replies to this request
                                                   ///< should be sent to this
                                                   ///< address.
    bool hasManifest;   ///< True if the header holds a piggy-backed manifest.
    Manifest manifest;  ///< A piggy-backed manifest (may be unfilled).

    /// RequestHeader default constructor.
    RequestHeader()
        : common(Opcode::Request)
        , rooId()
        , requestId()
        , replyAddress()
        , hasManifest(false)
        , manifest()
    {}

    /// RequestHeader constructor.
    explicit RequestHeader(RooId rooId, RequestId requestId)
        : common(Opcode::Request)
        , rooId(rooId)
        , requestId(requestId)
        , replyAddress()
        , hasManifest(false)
        , manifest()
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
    bool manifestImplied;   ///< True if receipt of this response should be
                            ///< considered receipt of a Manifest for the
                            ///< associated branch. False, otherwise.
    bool hasManifest;   ///< True if the header holds a piggy-backed manifest.
    Manifest manifest;  ///< A piggy-backed manifest (may be unfilled).

    /// ResponseHeader default constructor.
    ResponseHeader()
        : common(Opcode::Response)
        , rooId()
        , branchId()
        , responseId()
        , manifestImplied()
        , hasManifest(false)
        , manifest()
    {}

    /// ResponseHeader constructor.
    explicit ResponseHeader(RooId rooId, BranchId branchId,
                            ResponseId responseId, bool manifestImplied = false)
        : common(Opcode::Response)
        , rooId(rooId)
        , branchId(branchId)
        , responseId(responseId)
        , manifestImplied(manifestImplied)
        , hasManifest(false)
        , manifest()
    {}
} __attribute__((packed));

/**
 * Describes the wire format for a task Manifest message header.
 */
struct ManifestHeader {
    HeaderCommon common;    ///< Common header information.
    RooId rooId;            ///< Id of the RooPC to which this message belongs.
    uint8_t manifestCount;  ///< Number of manifests contained in this message.

    /// Header default constructor.
    ManifestHeader()
        : common(Opcode::Manifest)
        , rooId()
        , manifestCount(0)
    {}

    /// Header constructor.
    explicit ManifestHeader(RooId rooId, uint8_t manifestCount)
        : common(Opcode::Manifest)
        , rooId(rooId)
        , manifestCount(manifestCount)
    {}
} __attribute__((packed));

/**
 * Describes the wire format for a Ping message header.
 */
struct PingHeader {
    HeaderCommon common;  ///< Common header information.
    RequestId requestId;  ///< RequestId of the task that should receive this
                          ///< ping message.

    /// Header default constructor.
    PingHeader()
        : common(Opcode::Ping)
        , requestId()
    {}

    /// Header constructor.
    explicit PingHeader(RequestId requestId)
        : common(Opcode::Ping)
        , requestId(requestId)
    {}

} __attribute__((packed));

/**
 * Describes the wire format for a Pong message header.
 */
struct PongHeader {
    HeaderCommon common;     ///< Common header information.
    RooId rooId;             ///< Id of the associated RooPC.
    RequestId requestId;     ///< Id of the request that was pinged.
    TaskId taskId;           ///< Id of the responding task.
    uint32_t requestCount;   ///< Number of requests delegated by this task.
    uint32_t responseCount;  ///< Number of responses this task sent.
    bool taskComplete;       ///< True if the responding task has finished
                             ///< processing the returned request and response
                             ///< counts are final.
    bool branchComplete;     ///< True if the task terminates a branch and
                             ///< and its manifest can be marked received.
    // The header is followed by an area follow by an array of WireFormatAddress
    // entries. The first entry is Each entry holds the
    // destination address of a delegated request with the entries ordered by
    // increasing BranchId.

    /// Header default constructor.
    PongHeader()
        : common(Opcode::Pong)
        , rooId()
        , requestId()
        , taskId()
        , requestCount()
        , responseCount()
        , taskComplete(false)
        , branchComplete(false)
    {}

    /// Header constructor.
    explicit PongHeader(RooId rooId, RequestId requestId, TaskId taskId,
                        uint32_t requestCount, uint32_t responseCount,
                        bool taskComplete, bool branchComplete)
        : common(Opcode::Pong)
        , rooId(rooId)
        , requestId(requestId)
        , taskId(taskId)
        , requestCount(requestCount)
        , responseCount(responseCount)
        , taskComplete(taskComplete)
        , branchComplete(branchComplete)
    {}
} __attribute__((packed));

/**
 * Describes the wire format for a Error message header.
 */
struct ErrorHeader {
    HeaderCommon common;  ///< Common header information.
    RooId rooId;          ///< Id of the RooPC to which this message belongs.

    /// Header default constructor.
    ErrorHeader()
        : common(Opcode::Error)
        , rooId()
    {}

    /// Header constructor.
    explicit ErrorHeader(RooId rooId)
        : common(Opcode::Error)
        , rooId(rooId)
    {}
} __attribute__((packed));

}  // namespace Proto
}  // namespace Roo

#endif  // ROO_PROTO_H
