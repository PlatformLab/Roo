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
 * A unique identifier for a RooPC.
 */
struct RooId {
    uint64_t sessionId;  ///< Uniquely identifies the client transport for
                         ///< this RooPC.
    uint64_t sequence;   ///< Sequence number for this RooPC (unique for
                         ///< sessionId, monotonically increasing).

    /// RooId default constructor.
    RooId()
        : sessionId(0)
        , sequence(0)
    {}

    /// RooId constructor.
    RooId(uint64_t sessionId, uint64_t sequence)
        : sessionId(sessionId)
        , sequence(sequence)
    {}

    /**
     * Comparison function for RooId, for use in std::maps etc.
     */
    bool operator<(RooId other) const
    {
        return (sessionId < other.sessionId) ||
               ((sessionId == other.sessionId) && (sequence < other.sequence));
    }

    /**
     * Equality function for RooId, for use in std::unordered_maps etc.
     */
    bool operator==(RooId other) const
    {
        return ((sessionId == other.sessionId) && (sequence == other.sequence));
    }

    /**
     * This class computes a hash of an RooId, so that RooId can be used
     * as keys in unordered_maps.
     */
    struct Hasher {
        /// Return a "hash" of the given RooId.
        std::size_t operator()(const RooId& rooId) const
        {
            std::size_t h1 = std::hash<uint64_t>()(rooId.sessionId);
            std::size_t h2 = std::hash<uint64_t>()(rooId.sequence);
            return h1 ^ (h2 << 1);
        }
    };
} __attribute__((packed));

/**
 * A unique identifier for a Request.
 */
struct RequestId {
    uint64_t sessionId;  ///< Unique id for session that sent this request.
    uint64_t sequence;   ///< Sequence number for this request (unique for
                         ///< sessionId, monotonically increasing).

    /// RequestId default constructor.
    RequestId()
        : sessionId(0)
        , sequence(0)
    {}

    /// RequestId constructor.
    RequestId(uint64_t sessionId, uint64_t sequence)
        : sessionId(sessionId)
        , sequence(sequence)
    {}

    /**
     * Comparison function for RequestId, for use in std::maps etc.
     */
    bool operator<(RequestId other) const
    {
        return (sessionId < other.sessionId) ||
               ((sessionId == other.sessionId) && (sequence < other.sequence));
    }

    /**
     * Equality function for RequestId, for use in std::unordered_maps etc.
     */
    bool operator==(RequestId other) const
    {
        return ((sessionId == other.sessionId) && (sequence == other.sequence));
    }

    /**
     * This class computes a hash of an RequestId, so that Id can be used
     * as keys in unordered_maps.
     */
    struct Hasher {
        /// Return a "hash" of the given RequestId.
        std::size_t operator()(const RequestId& requestId) const
        {
            std::size_t h1 = std::hash<uint64_t>()(requestId.sessionId);
            std::size_t h2 = std::hash<uint64_t>()(requestId.sequence);
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
    Message = 1,
    Delegation,
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
 * Contains the header definitions for sending and receiving an application
 * level Message; one RooPC will involve the sending and receiving of two or
 * more messages.
 */
namespace Message {

/**
 * Distinguishes between different types of RooPC messages.
 */
enum class Type : uint8_t {
    Initial,   ///< Request sent directly from the RooPC client
    Request,   ///< Request sent by an intermediate ServerTask
    Response,  ///< Response sent to the RooPC client
    Invalid,   ///< No message should be of this type
};

/**
 * Describes the wire format for header fields for all Message.
 */
struct Header {
    HeaderCommon common;  ///< Common header information.
    RooId rooId;          ///< Id of the RooPC to which this message belongs.
    RequestId requestId;  ///< Id of this message, if this message is a request;
                          ///< or, id of the request to which this message
                          ///< responds, if this message is a response.
    Type type;            ///< Identifies what the type of the message
    Homa::Driver::WireFormatAddress
        replyAddress;  ///< Replies to this Message should
                       ///< be sent to this address.

    /// Header default constructor.
    Header()
        : common(Opcode::Message)
        , rooId()
        , requestId()
        , type(Type::Invalid)
        , replyAddress()
    {}

    /// Header constructor.
    explicit Header(RooId rooId, RequestId requestId, Type type)
        : common(Opcode::Message)
        , rooId(rooId)
        , requestId(requestId)
        , type(type)
        , replyAddress()
    {}

} __attribute__((packed));

}  // namespace Message

namespace Delegation {

/**
 * Describes the wire format for the delegation message header
 */
struct Header {
    HeaderCommon common;  ///< Common header information.
    RooId rooId;          ///< Id of the RooPC to which this message belongs.
    RequestId requestId;  ///< Id of this message, if this message is a request;
                          ///< or, id of the request to which this message
                          ///< responds, if this message is a response.
    uint64_t num;         ///< Number of delegated requests
    /// An array of RequestId entries follows this header.

    /// Header default constructor.
    Header()
        : common(Opcode::Delegation)
        , rooId()
        , requestId()
        , num(0)
    {}

    /// Header constructor.
    explicit Header(RooId rooId, RequestId requestId, uint64_t num)
        : common(Opcode::Delegation)
        , rooId(rooId)
        , requestId(requestId)
        , num(num)
    {}
} __attribute__((packed));

}  // namespace Delegation

}  // namespace Proto
}  // namespace Roo

#endif  // ROO_PROTO_H
