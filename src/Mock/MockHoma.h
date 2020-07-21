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

#ifndef ROO_MOCK_MOCKHOMA_H
#define ROO_MOCK_MOCKHOMA_H

#include <Homa/Homa.h>
#include <gmock/gmock.h>

namespace Roo {
namespace Mock {
namespace Homa {

/**
 * MockDriver is a gmock supported mock driver implementation that is used
 * in unit testing.
 */
class MockDriver : public ::Homa::Driver {
  public:
    /**
     * Used in unit tests to mock calls to Driver::Packet.
     */
    class MockPacket : public Driver::Packet {
      public:
        MockPacket(void* payload, uint16_t length = 0)
            : Packet(payload, length)
        {}

        MOCK_METHOD0(getMaxPayloadSize, int());
    };

    MOCK_METHOD1(getAddress, Address(std::string const* const addressString));
    MOCK_METHOD1(getAddress,
                 Address(WireFormatAddress const* const wireAddress));
    MOCK_METHOD1(addressToString, std::string(Address address));
    MOCK_METHOD2(addressToWireFormat,
                 void(Address address, WireFormatAddress* wireAddress));
    MOCK_METHOD0(allocPacket, Packet*());
    MOCK_METHOD1(sendPacket, void(Packet* packet));
    MOCK_METHOD0(flushPackets, void());
    MOCK_METHOD2(receivePackets,
                 uint32_t(uint32_t maxPackets, Packet* receivedPackets[]));
    MOCK_METHOD2(releasePackets, void(Packet* packets[], uint16_t numPackets));
    MOCK_METHOD0(getHighestPacketPriority, int());
    MOCK_METHOD0(getMaxPayloadSize, uint32_t());
    MOCK_METHOD0(getBandwidth, uint32_t());
    MOCK_METHOD0(getLocalAddress, Address());
    MOCK_METHOD0(getQueuedBytes, uint32_t());
};

/**
 * Mock implementation of Homa::InMessage that is used in unit testing.
 */
class MockInMessage : public ::Homa::InMessage {
  public:
    MOCK_CONST_METHOD0(acknowledge, void());
    MOCK_CONST_METHOD0(dropped, bool());
    MOCK_CONST_METHOD0(fail, void());
    MOCK_CONST_METHOD3(get, std::size_t(size_t offset, void* destination,
                                        std::size_t count));
    MOCK_CONST_METHOD0(length, std::size_t());
    MOCK_METHOD1(strip, void(size_t count));

  protected:
    MOCK_METHOD0(release, void());
};

/**
 * Mock implementation of Homa::OutMessage that is used in unit testing.
 */
class MockOutMessage : public ::Homa::OutMessage {
  public:
    MOCK_METHOD2(append, void(const void* source, std::size_t count));
    MOCK_METHOD0(cancel, void());
    MOCK_CONST_METHOD0(getStatus, Status());
    MOCK_CONST_METHOD0(length, std::size_t());
    MOCK_METHOD2(prepend, void(const void* source, std::size_t count));
    MOCK_METHOD1(reserve, void(size_t count));
    MOCK_METHOD2(send, void(::Homa::Driver::Address destination,
                            ::Homa::OutMessage::Options options));

  protected:
    MOCK_METHOD0(release, void());
};

/**
 * Mock implementation of Homa::Transport that is used in unit testing
 */
class MockTransport : public ::Homa::Transport {
  public:
    MOCK_METHOD0(alloc, ::Homa::unique_ptr<::Homa::OutMessage>());
    MOCK_METHOD0(receive, ::Homa::unique_ptr<::Homa::InMessage>());
    MOCK_METHOD0(poll, void());
    MOCK_METHOD0(getDriver, ::Homa::Driver*());
    MOCK_METHOD0(getId, uint64_t());
};

}  // namespace Homa
}  // namespace Mock
}  // namespace Roo

#endif  // ROO_MOCK_MOCKHOMA_H
