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
    uint64_t transportId;  ///< Uniquely identifies the client transport for
                           ///< this RooPC.
    uint64_t sequence;     ///< Sequence number for this RooPC (unique for
                           ///< transportId, monotonically increasing).

    /// RooId default constructor.
    RooId()
        : transportId(0)
        , sequence(0)
    {}

    /// RooId constructor.
    RooId(uint64_t transportId, uint64_t sequence)
        : transportId(transportId)
        , sequence(sequence)
    {}

    /**
     * Comparison function for RooId, for use in std::maps etc.
     */
    bool operator<(RooId other) const
    {
        return (transportId < other.transportId) ||
               ((transportId == other.transportId) &&
                (sequence < other.sequence));
    }

    /**
     * Equality function for RooId, for use in std::unordered_maps etc.
     */
    bool operator==(RooId other) const
    {
        return ((transportId == other.transportId) &&
                (sequence == other.sequence));
    }

    /**
     * This class computes a hash of an RooId, so that RooId can be used
     * as keys in unordered_maps.
     */
    struct Hasher {
        /// Return a "hash" of the given RooId.
        std::size_t operator()(const RooId& rooId) const
        {
            std::size_t h1 = std::hash<uint64_t>()(rooId.transportId);
            std::size_t h2 = std::hash<uint64_t>()(rooId.sequence);
            return h1 ^ (h2 << 1);
        }
    };
} __attribute__((packed));

/**
 * Contains the header definitions for a Homa Message; one RooPC will involve
 * the sending and receiving of two or more messages.
 */
namespace Message {

/// Identifier for the Message that contains a RooPC's initiating request
/// RooPC (sent by the client).
static const int32_t INITIAL_REQUEST_ID = 0;
/// Identifier for the Message that contains the final reply to the
/// initial request (sent to the client).
static const int32_t ULTIMATE_RESPONSE_ID = -1;

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
 * Describes the wire format for header fields for all Message.
 */
struct Header {
    HeaderPrefix prefix;  ///< Common to all versions of the protocol.
    RooId rooId;          ///< Id of the RooPC to which this message belongs.
    int32_t stageId;  ///< Uniquely identifies this Message within the set of
                      ///< messages that belong to the RooPC.
    Homa::Driver::WireFormatAddress
        replyAddress;  ///< Replies to this Message should
                       ///< be sent to this address.

    /// CommonHeader default constructor.
    Header()
        : prefix(1)
        , rooId()
        , stageId()
        , replyAddress()
    {}

    /// CommonHeader constructor.
    explicit Header(RooId rooId, int32_t stageId)
        : prefix(1)
        , rooId(rooId)
        , stageId(stageId)
        , replyAddress()
    {}
} __attribute__((packed));

}  // namespace Message
}  // namespace Proto
}  // namespace Roo

#endif  // ROO_PROTO_H
