module;

#include <vector>
#include <cstddef>

export module networks.core.packet_framer;

import std;
import networks.core.packet;
import commons.buffers.ibuffer;

namespace LibNetworks::Core
{

// TCP 스트림에서 패킷 단위로 분리
export class PacketFramer
{
public:
    PacketFramer() = default;

    // 수신 버퍼에서 완전한 패킷을 꺼내기
    // RETURN : 패킷 생성 성공 여부
    std::optional<Packet> TryPop(LibCommons::Buffers::IBuffer& rfReceiveBuffer)
    {
        const size_t canRead = rfReceiveBuffer.CanReadSize();
        if (canRead < Packet::GetHeaderSize())
        {
            return std::nullopt;
        }

        unsigned char header[Packet::GetHeaderSize()]{};
        if (!rfReceiveBuffer.Peek(header, Packet::GetHeaderSize()))
        {
            return std::nullopt;
        }

        const auto packetSize = Packet::GetHeaderFromBuffer(header);
        if (packetSize <= 0)
        {
            return std::nullopt;
        }

        if (canRead < static_cast<size_t>(packetSize))
        {
            return std::nullopt;
        }

        std::vector<unsigned char> buffers;
        buffers.resize(static_cast<size_t>(packetSize));

        if (!rfReceiveBuffer.Pop(buffers.data(), buffers.size()))
        {
            return std::nullopt;
        }

        return Packet(std::move(buffers));
    }
};

} // namespace LibNetworks::Core
