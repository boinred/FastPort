module;

#include <vector>
#include <cstddef>

export module networks.core.packet_framer;

import std;
import networks.core.packet;
import commons.buffers.ibuffer;

namespace LibNetworks::Core
{

export enum class PacketFrameResult
{
    Ok,
    NeedMore,
    Invalid
};

export struct PacketFrame
{
    PacketFrameResult Result = PacketFrameResult::NeedMore;
    std::optional<Packet> PacketOpt{};
};

// TCP 스트림에서 패킷 단위로 분리
export class PacketFramer
{
public:
    PacketFramer() = delete;

    static PacketFrame TryFrameFromBuffer(LibCommons::Buffers::IBuffer& rfReceiveBuffer)
    {
        const size_t canRead = rfReceiveBuffer.CanReadSize();
        if (canRead < Packet::GetHeaderSize())
        {
            return { PacketFrameResult::NeedMore, std::nullopt };
        }

        unsigned char header[Packet::GetHeaderSize()]{};
        if (!rfReceiveBuffer.Peek(std::as_writable_bytes(std::span(header))))
        {
            return { PacketFrameResult::Invalid, std::nullopt };
        }

        const uint16_t packetSize = Packet::GetHeaderFromBuffer(std::as_bytes(std::span(header)));
        const size_t minPacketSize = Packet::GetHeaderSize() + Packet::GetPacketIdSize();

        if (packetSize < minPacketSize)
        {
            return { PacketFrameResult::Invalid, std::nullopt };
        }

        // 2 bytes size field => practical upper bound
        if (packetSize > 0xFFFF)
        {
            return { PacketFrameResult::Invalid, std::nullopt };
        }

        if (canRead < static_cast<size_t>(packetSize))
        {
            return { PacketFrameResult::NeedMore, std::nullopt };
        }

        std::vector<unsigned char> buffers;
        buffers.resize(static_cast<size_t>(packetSize));

        if (!rfReceiveBuffer.Pop(std::as_writable_bytes(std::span(buffers))))
        {
            return { PacketFrameResult::Invalid, std::nullopt };
        }

        return { PacketFrameResult::Ok, Packet(std::move(buffers)) };
    }
};

} // namespace LibNetworks::Core
