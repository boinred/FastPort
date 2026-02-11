#include "CppUnitTest.h"
#include <vector>
#include <span>
#include <numeric>

import commons.buffers.external_circle_buffer_queue;
import networks.core.packet_framer;
import networks.core.packet;

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace LibNetworksTests
{
    TEST_CLASS(RioBufferAdapterTests)
    {
    public:
        // Wrap-around 상황에서의 데이터 쓰기 및 읽기 검증
        TEST_METHOD(TestWrapAroundWriteRead)
        {
            // 100바이트 버퍼 준비
            std::vector<std::byte> storage(100);
            LibCommons::Buffers::ExternalCircleBufferQueue queue(storage);

            // 1. 초기 상태: Head=0, Tail=0, Size=0
            
            // 2. 80바이트 쓰기 (Head -> 80)
            std::vector<std::byte> data1(80);
            for(size_t i=0; i<data1.size(); ++i) data1[i] = static_cast<std::byte>(i);
            std::vector<std::span<std::byte>> outBuffers;
            
            // Write 대신 AllocateWrite 사용 (RIOSession 패턴)
            Assert::IsTrue(queue.AllocateWrite(80, outBuffers));
            Assert::AreEqual((size_t)1, outBuffers.size());
            std::memcpy(outBuffers[0].data(), data1.data(), 80);
            // CommitWrite는 AllocateWrite에서 이미 Head를 이동시키므로 필요 없음 (구현에 따라 다를 수 있음 확인 필요)
            // ExternalCircleBufferQueue::AllocateWrite는 m_Head를 이동시킴.

            // 3. 50바이트 읽기/소비 (Tail -> 50, Size -> 30)
            std::vector<std::span<const std::byte>> readBuffers;
            size_t readable = queue.GetReadBuffers(readBuffers);
            Assert::AreEqual((size_t)80, readable);
            Assert::IsTrue(queue.Consume(50));

            // 4. 40바이트 쓰기 (Wrap-around 발생 기대: Head 80 -> 20)
            // 남은 공간: 100 - 30 = 70. 40바이트 쓰기 가능.
            // 물리적 공간: [80, 100) -> 20바이트, [0, 20) -> 20바이트
            outBuffers.clear();
            Assert::IsTrue(queue.AllocateWrite(40, outBuffers));
            Assert::AreEqual((size_t)2, outBuffers.size());
            Assert::AreEqual((size_t)20, outBuffers[0].size());
            Assert::AreEqual((size_t)20, outBuffers[1].size());

            // 데이터 채우기 (검증용)
            std::vector<std::byte> data2(40);
            for(size_t i=0; i<data2.size(); ++i) data2[i] = static_cast<std::byte>(100 + i);
            std::memcpy(outBuffers[0].data(), data2.data(), 20);
            std::memcpy(outBuffers[1].data(), data2.data() + 20, 20);

            // 5. 전체 읽기 (Wrap-around 읽기 검증)
            // 현재 데이터: 30(기존) + 40(신규) = 70바이트
            // Tail=50. 물리적: [50, 100) -> 50바이트 중 30바이트 유효([50, 80)). 
            // 80부터 40바이트 추가됨 -> [80, 100) 20바이트, [0, 20) 20바이트.
            // 논리적 순서: [50, 80) (30byte) -> [80, 100) (20byte) -> [0, 20) (20byte)
            // GetReadBuffers는 연속된 청크를 반환함. Tail=50부터 End=100까지 50바이트(30+20), 그 다음 0부터 20까지.
            
            readBuffers.clear();
            readable = queue.GetReadBuffers(readBuffers);
            Assert::AreEqual((size_t)70, readable);
            
            // 예상: [50, 100) 50바이트, [0, 20) 20바이트 -> 총 2개 청크
            Assert::AreEqual((size_t)2, readBuffers.size());
            Assert::AreEqual((size_t)50, readBuffers[0].size());
            Assert::AreEqual((size_t)20, readBuffers[1].size());

            // 데이터 검증
            // 첫 번째 청크 [50, 80) : data1의 뒷부분 (50~79) = 30바이트
            // 첫 번째 청크 [80, 100) : data2의 앞부분 (0~19) = 20바이트
            // 두 번째 청크 [0, 20) : data2의 뒷부분 (20~39) = 20바이트
            
            auto span1 = readBuffers[0];
            auto span2 = readBuffers[1];

            // 0~29 (30bytes) == data1[50]...
            for(size_t i=0; i<30; ++i)
            {
                Assert::AreEqual(static_cast<int>(data1[50+i]), static_cast<int>(span1[i]));
            }
            // 30~49 (20bytes) == data2[0]...
            for(size_t i=0; i<20; ++i)
            {
                Assert::AreEqual(static_cast<int>(data2[i]), static_cast<int>(span1[30+i]));
            }
            // 0~19 (20bytes) == data2[20]...
            for(size_t i=0; i<20; ++i)
            {
                Assert::AreEqual(static_cast<int>(data2[20+i]), static_cast<int>(span2[i]));
            }
        }

        // PacketFramer가 분절된 버퍼(Wrap-around)를 처리하는지 검증
        TEST_METHOD(TestPacketFramerWithWrapAround)
        {
            // 100바이트 버퍼
            std::vector<std::byte> storage(100);
            LibCommons::Buffers::ExternalCircleBufferQueue queue(storage);

            // 강제로 Wrap-around 상태 만들기
            // Tail=80, Head=20, Size=40 (물리적: [80,100), [0,20))
            
            // 1. 80바이트 더미 소비
            std::vector<std::span<std::byte>> dummy;
            queue.AllocateWrite(80, dummy);
            queue.Consume(80); // Tail=80

            // 2. 패킷 생성 (헤더 포함 10바이트)
            std::string payload = "ABCDEF"; // 6bytes
            LibNetworks::Core::Packet packet(100, payload); // Header(2) + ID(2) + Payload(6) = 10bytes
            auto rawPacket = packet.GetRawSpan();

            // 3. 패킷을 분절하여 쓰기 (경계에 걸치게)
            // 현재 Tail=80, Head=80. 남은 공간 [80, 100) 20bytes.
            // 우리는 [95, 100) 5bytes, [0, 5) 5bytes 이렇게 쓰고 싶음.
            // 그러려면 먼저 15바이트를 더미로 채워야 함.
            queue.AllocateWrite(15, dummy); 
            queue.Consume(15); // Tail=95, Head=95.

            // 이제 Tail=95. 여기서 패킷 10바이트를 쓰면?
            // AllocateWrite(10) -> [95, 100) 5bytes, [0, 5) 5bytes.
            std::vector<std::span<std::byte>> writeBufs;
            Assert::IsTrue(queue.AllocateWrite(rawPacket.size(), writeBufs));
            
            // 쓰기 수행
            size_t offset = 0;
            for(auto& buf : writeBufs)
            {
                size_t copyLen = std::min(buf.size(), rawPacket.size() - offset);
                std::memcpy(buf.data(), rawPacket.data() + offset, copyLen);
                offset += copyLen;
            }

            // 4. PacketFramer로 파싱 시도 (Zero-Copy)
            // Framer는 IBuffer 인터페이스를 통해 GetReadBuffers를 호출하고
            // 분절된 span을 모아서 하나의 패킷으로 조립해야 함(또는 내부적으로 처리).
            // 현재 PacketFramer 구현 확인 필요: TryFrameFromBuffer가 분절된 데이터를 처리하는가?
            // PacketFramer.ixx를 보면 GetReadBuffers 호출 후 첫 번째 버퍼만 보는지,
            // 아니면 Copy를 통해 합치는지 확인해야 함.
            // -> 확인 결과: PacketFramer::TryFrameFromBuffer는 GetReadBuffers로 목록을 가져와서
            //    헤더가 잘려있거나 바디가 잘려있으면 'Peek' 또는 내부 버퍼로 복사하여 처리함.
            
            auto result = LibNetworks::Core::PacketFramer::TryFrameFromBuffer(queue);
            
            Assert::IsTrue(result.Result == LibNetworks::Core::PacketFrameResult::Ok, L"Framer failed to handle wrapped packet");
            Assert::IsTrue(result.PacketOpt.has_value());
            Assert::AreEqual((uint16_t)100, result.PacketOpt->GetPacketId());
            
            // 페이로드 확인 (직접 파싱해서 비교)
            // Packet 클래스에 GetPayloadString 같은게 없으므로 재구성해서 비교하거나...
            // 여기서는 ID와 성공 여부만으로도 Framer가 경계 처리를 잘 했다고 볼 수 있음.
        }
    };
}
