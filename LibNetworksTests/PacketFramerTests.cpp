#include "CppUnitTest.h"
#include <vector>
#include <cstdint>
#include <array>
#include <span>
#include <WinSock2.h> // For htons

#pragma comment(lib, "ws2_32.lib")

 import std; 

import networks.core.packet_framer;
import networks.core.packet;
import commons.buffers.circle_buffer_queue;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace LibNetworks::Core;
using namespace LibCommons::Buffers;

namespace LibNetworksTests
{
	TEST_CLASS(PacketFramerTests)
	{
	public:

		// 빈 버퍼에서 패킷 생성을 시도할 때 NeedMore가 반환되는지 확인
		TEST_METHOD(EmptyBuffer_ReturnsNeedMore)
		{
			CircleBufferQueue buffer(1024);
			auto result = PacketFramer::TryFrameFromBuffer(buffer);

			Assert::IsTrue(result.Result == PacketFrameResult::NeedMore);
			Assert::IsFalse(result.PacketOpt.has_value());
		}

		// 헤더 크기(2바이트)보다 작은 데이터가 있을 때 NeedMore가 반환되는지 확인
		TEST_METHOD(PartialHeader_ReturnsNeedMore)
		{
			CircleBufferQueue buffer(1024);
			
			// 1 byte only (Header needs 2)
			std::array<uint8_t, 1> partial = { 0x01 };
			buffer.Write(std::as_bytes(std::span(partial)));

			auto result = PacketFramer::TryFrameFromBuffer(buffer);

			Assert::IsTrue(result.Result == PacketFrameResult::NeedMore);
		}

		// 헤더는 유효하지만 바디 데이터가 부족할 때 NeedMore가 반환되는지 확인
		TEST_METHOD(ValidHeader_InsufficientBody_ReturnsNeedMore)
		{
			CircleBufferQueue buffer(1024);

			// Header says size = 10. (2 Header + 2 ID + 6 Payload)
			uint16_t size = htons(10);
			
			buffer.Write(std::as_bytes(std::span(&size, 1))); // Only Size field

			// Check after size write (Still NeedMore because waiting for full packet)
			PacketFrame result = PacketFramer::TryFrameFromBuffer(buffer);
			Assert::IsTrue(result.Result == PacketFrameResult::NeedMore);
			
			// Write PacketID
			uint16_t id = htons(1001);
			buffer.Write(std::as_bytes(std::span(&id, 1)));

			// Write partial payload (3 bytes out of 6)
			// "123" -> {'1', '2', '3'}
			std::array<char, 3> payload = { '1', '2', '3' };
			
			buffer.Write(std::as_bytes(std::span(payload)));

			// Total in buffer: 2 + 2 + 3 = 7 bytes. Needed 10.
			PacketFrame result_2 = PacketFramer::TryFrameFromBuffer(buffer);
			Assert::IsTrue(result_2.Result == PacketFrameResult::NeedMore);
		}

		// 완전한 패킷 데이터가 있을 때 Ok가 반환되고 패킷이 정상적으로 생성되는지 확인
		TEST_METHOD(CompletePacket_ReturnsOk)
		{
			CircleBufferQueue buffer(1024);

			// Size: 2(Header) + 2(ID) + 5(Payload) = 9
			uint16_t totalSize = 9;
			uint16_t sizeNet = htons(totalSize);
			uint16_t idNet = htons(1001);
			std::array<char, 5> payload = { 'H', 'e', 'l', 'l', 'o' };

			buffer.Write(std::as_bytes(std::span(&sizeNet, 1)));
			buffer.Write(std::as_bytes(std::span(&idNet, 1)));
			buffer.Write(std::as_bytes(std::span(payload)));

			auto result = PacketFramer::TryFrameFromBuffer(buffer);

			Assert::IsTrue(result.Result == PacketFrameResult::Ok);
			Assert::IsTrue(result.PacketOpt.has_value());

			const auto& packet = result.PacketOpt.value();
			Assert::AreEqual((int)1001, (int)packet.GetPacketId());
			Assert::AreEqual((int)5, (int)packet.GetPayloadSize());
			
			// Buffer should be empty now
			Assert::AreEqual((size_t)0, buffer.CanReadSize());
		}

		// 최소 패킷 크기(헤더+ID)보다 작은 크기가 명시된 경우 Invalid가 반환되는지 확인
		TEST_METHOD(PacketTooSmall_ReturnsInvalid)
		{
			CircleBufferQueue buffer(1024);

			// Size 3 (Header 2 + 1 byte). Minimum is 4 (Header 2 + ID 2)
			uint16_t sizeNet = htons(3);
			buffer.Write(std::as_bytes(std::span(&sizeNet, 1)));
			
			
			// Add dummy data make it readable
			std::array<char, 1> dummy = { '1' };
			buffer.Write(std::as_bytes(std::span(dummy)));

			auto result = PacketFramer::TryFrameFromBuffer(buffer);

			Assert::IsTrue(result.Result == PacketFrameResult::Invalid);
		}

		// 패킷 크기가 0으로 명시된 경우 Invalid가 반환되는지 확인
		TEST_METHOD(PacketSizeZero_ReturnsInvalid)
		{
			CircleBufferQueue buffer(1024);

			uint16_t sizeNet = htons(0);
			buffer.Write(std::as_bytes(std::span(&sizeNet, 1)));

			// Need logic to ensure we don't just wait forever if size is 0 and valid enough to be read? 
			// Actually PacketFramer checks size < minPacketSize (4). So 0 < 4 -> Invalid.
			
			// However, Framer peeks header first. If buffer has 2 bytes (size=0), it reads Size=0.
			// Then checks if (packetSize < minPacketSize).
			
			auto result = PacketFramer::TryFrameFromBuffer(buffer);
			Assert::IsTrue(result.Result == PacketFrameResult::Invalid);
		}

		/*
		TEST_METHOD(PacketTooLarge_ReturnsInvalid)
		{
			// Note: Current IBuffer/CircleBuffer might limit capacity, 
			// but PacketFramer Logic checks > 0xFFFF explicitly if implemented.
			// Let's assume PacketFramer checks against 0xFFFF or some max size.
			
			CircleBufferQueue buffer(65536); // Large buffer
			
			// Current logic: if (packetSize > 0xFFFF) return Invalid; (Checked in code)
			// Since uint16_t max is 0xFFFF, this condition is only reachable if logic casts incorrectly 
			// OR if we treat packetSize field as possibly larger protocol logic wise, 
			// BUT PacketFramer reads uint16_t. So PacketSize can never exceed 0xFFFF mathematically from reading 2 bytes.
			// Wait, the check in PacketFramer.ixx is: 
			// const uint16_t packetSize = ...;
			// if (packetSize > 0xFFFF) ... 
			// This condition is always false for uint16_t.
			// However, if the intention was to limit to a smaller MAX_PACKET_SIZE, we can test that.
			// For now, let's skip "TooLarge" unless we see a specific constant constraint like 8KB.
		}
		*/

		// 버퍼에 여러 패킷이 연속으로 있을 때 순차적으로 처리가 되는지 확인
		TEST_METHOD(MultiplePackets_ProcessedSequentially)
		{
			CircleBufferQueue buffer(1024);

			// Packet 1: ID 10, Payload "A" (size 2+2+1 = 5)
			std::vector<char> p1;
			uint16_t s1 = htons(5);
			uint16_t id1 = htons(10);
			p1.push_back(((char*)&s1)[0]); p1.push_back(((char*)&s1)[1]);
			p1.push_back(((char*)&id1)[0]); p1.push_back(((char*)&id1)[1]);
			p1.push_back('A');

			// Packet 2: ID 20, Payload "BB" (size 2+2+2 = 6)
			std::vector<char> p2;
			uint16_t s2 = htons(6);
			uint16_t id2 = htons(20);
			p2.push_back(((char*)&s2)[0]); p2.push_back(((char*)&s2)[1]);
			p2.push_back(((char*)&id2)[0]); p2.push_back(((char*)&id2)[1]);
			p2.push_back('B'); p2.push_back('B');

			// Write P1
			buffer.Write(std::as_bytes(std::span(p1)));
			// Write P2
			buffer.Write(std::as_bytes(std::span(p2)));

			// Process P1
			auto res1 = PacketFramer::TryFrameFromBuffer(buffer);
			Assert::IsTrue(res1.Result == PacketFrameResult::Ok);
			Assert::AreEqual((int)10, (int)res1.PacketOpt->GetPacketId());

			// Process P2
			auto res2 = PacketFramer::TryFrameFromBuffer(buffer);
			Assert::IsTrue(res2.Result == PacketFrameResult::Ok);
			Assert::AreEqual((int)20, (int)res2.PacketOpt->GetPacketId());

			// Empty
			auto res3 = PacketFramer::TryFrameFromBuffer(buffer);
			Assert::IsTrue(res3.Result == PacketFrameResult::NeedMore);
		}
	};
}
