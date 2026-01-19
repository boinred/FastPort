module;

#include <vector>
#include <cstdint>
#include <WinSock2.h>

export module networks.core.packet;

import std;

namespace LibNetworks::Core
{

/**
 * Packet
 * [HEADER]        [PAYLOAD]
 * [2 bytes - Size][2 bytes - Packet ID][N bytes - Payload]
 */
export class Packet
{
public:
    Packet() = delete;

    // Raw 버퍼 기반 패킷 생성(버퍼 소유권 이동)
    Packet(std::vector<unsigned char>&& buffers)
        : m_PacketId(GetPacketIdFromBuffer(buffers.data())),
          m_Payload(reinterpret_cast<const char*>(buffers.data() + GetHeaderSize() + GetPacketIdSize()),
                   buffers.size() - GetHeaderSize() - GetPacketIdSize()),
          m_RawBinaries(std::move(buffers))
    {
    }

    Packet(uint16_t packetId, std::string content)
        : m_PacketId(static_cast<int16_t>(packetId)), m_Payload(std::move(content))
    {
        MakeRawBinaries();
    }

    Packet(uint16_t packetId, const unsigned char* pData, size_t sizeOfData)
        : m_PacketId(static_cast<int16_t>(packetId)), m_Payload(reinterpret_cast<const char*>(pData), sizeOfData)
    {
        MakeRawBinaries();
    }

    inline static constexpr size_t GetHeaderSize() { return sizeof(uint16_t); }
    inline static constexpr size_t GetPacketIdSize() { return sizeof(uint16_t); }

    inline static uint16_t GetHeaderFromBuffer(const unsigned char* pData)
    {
        if (!pData)
        {
            return 0;
        }

        uint16_t net = 0;
        std::memcpy(&net, pData, sizeof(net));
        return ntohs(net);
    }

    inline static uint16_t GetPacketIdFromBuffer(const unsigned char* pData)
    {
        if (!pData)
        {
            return 0;
        }

        uint16_t net = 0;
        std::memcpy(&net, pData + GetHeaderSize(), sizeof(net));
        return ntohs(net);
    }

    constexpr size_t GetPacketSize() const { return m_Payload.size() + GetHeaderSize() + GetPacketIdSize(); }

    constexpr uint16_t GetPacketId() const { return static_cast<uint16_t>(m_PacketId); }

    constexpr uint16_t GetPayloadSize() const { return static_cast<uint16_t>(m_Payload.size()); }

    const unsigned char* GetRawData() const { return m_RawBinaries.data(); }

    uint16_t GetPacketIdFromRawBinaries() const
    {
        if (m_RawBinaries.size() < GetHeaderSize() + GetPacketIdSize())
        {
            return 0;
        }

        return GetPacketIdFromBuffer(m_RawBinaries.data());
    }

    template<class T>
    bool ParseMessage(T& outMessage) const
    {
        return outMessage.ParseFromString(m_Payload);
    }

private:
    void MakeRawBinaries()
    {
        const uint16_t size = static_cast<uint16_t>(GetPacketSize());
        const uint16_t packetId = GetPacketId();

        m_RawBinaries.clear();
        m_RawBinaries.reserve(size);

        const uint16_t sizeNet = htons(size);
        const uint16_t idNet = htons(packetId);

        const unsigned char* sizeBytes = reinterpret_cast<const unsigned char*>(&sizeNet);
        const unsigned char* idBytes = reinterpret_cast<const unsigned char*>(&idNet);

        m_RawBinaries.insert(m_RawBinaries.end(), sizeBytes, sizeBytes + sizeof(sizeNet));
        m_RawBinaries.insert(m_RawBinaries.end(), idBytes, idBytes + sizeof(idNet));
        m_RawBinaries.insert(m_RawBinaries.end(), m_Payload.begin(), m_Payload.end());
    }

private:
    const int16_t m_PacketId = 0;
    const std::string m_Payload{};
    std::vector<unsigned char> m_RawBinaries{};
};

} // namespace LibNetworks::Core
