module;

#include <vector>

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

    Packet(const unsigned short packetId, std::string content)
        : m_PacketId(packetId), m_Payload(std::move(content))
    {
        MakeRawBinaries();
    }

    Packet(const short packetId, const unsigned char* pData, const int sizeOfData)
        : m_PacketId(packetId), m_Payload(reinterpret_cast<const char*>(pData), sizeOfData)
    {
        MakeRawBinaries();
    }

    inline static constexpr size_t GetHeaderFromBuffer(const unsigned char* pData)
    {
        if (!pData)
        {
            return 0;
        }
        return (pData[0] << 0) | (pData[1] << 8);
    }

    inline static constexpr short GetPacketIdFromBuffer(const unsigned char* pData)
    {
        if (!pData)
        {
            return 0;
        }
        const int C_PACKET_ID_INDEX = 2; // 패킷 ID 시작 위치(사이즈 2바이트 이후)
        return (pData[C_PACKET_ID_INDEX] << 0) | (pData[C_PACKET_ID_INDEX + 1] << 8);
    }

    // 패킷 크기 필드 크기(2바이트)
    inline static constexpr size_t GetHeaderSize() { return sizeof(short); }

    // 패킷 ID 필드 크기(2바이트)
    inline static constexpr size_t GetPacketIdSize() { return sizeof(short); }

    // 전체 패킷 크기(헤더+ID+페이로드)
    constexpr size_t GetPacketSize() const { return m_Payload.size() + GetHeaderSize() + GetPacketIdSize(); }

    // GET: 패킷 ID 
    constexpr short GetPacketId() const { return m_PacketId; }

    // GET : 페이로드 크기
    constexpr short GetPayloadSize() const { return static_cast<short>(m_Payload.size()); }

    // GET : 페이로드 데이터
    const unsigned char* GetRawData() const { return m_RawBinaries.data(); }

    // Raw 바이너리에서 패킷 ID 추출
    const short GetPacketIdFromRawBinaries() const
    {
        if (m_RawBinaries.size() < GetPacketIdSize())
        {
            return 0; // 유효하지 않은 크기
        }

        return GetPacketIdFromBuffer(m_RawBinaries.data());
    }

    template<class T>
    bool ParseMessage(T& outMessage) const
    {
        if (!outMessage.ParseFromString(m_Payload))
        {
            return false;
        }

        return true;
    };


private:
    // Raw 바이너리 생성
    void MakeRawBinaries()
    {
        const size_t size = GetPacketSize();
        const short packetId = GetPacketId();

        m_RawBinaries.push_back((size & 0x00ff) >> 0);
        m_RawBinaries.push_back((size & 0xff00) >> 8);

        m_RawBinaries.push_back((packetId & 0x00ff) >> 0);
        m_RawBinaries.push_back((packetId & 0xff00) >> 8);

        m_RawBinaries.insert(m_RawBinaries.end(), m_Payload.begin(), m_Payload.end());
    }


private:
    // 패킷 ID
    const short m_PacketId = 0;

    // 페이로드 데이터
    const std::string m_Payload{};

    // 전송용 Raw 바이너리(사이즈+ID+페이로드)
    std::vector<unsigned char> m_RawBinaries{};
};

} // namespace LibNetworks::Core
