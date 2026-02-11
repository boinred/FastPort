module;

#include <cstdint>
#include <WinSock2.h>
#include <google/protobuf/message.h>

export module benchmark.session;

import std;
import networks.sessions.inetwork_session;
import networks.sessions.outbound_session;
import networks.sessions.io_session;
import networks.sessions.rio_session;
import commons.buffers.ibuffer;
import networks.core.packet;
import networks.core.socket;
import networks.core.rio_buffer_manager;
import networks.core.rio_context; 

namespace FastPortBenchmark
{
    using namespace std;

// 벤치마크 세션 인터페이스
export class IBenchmarkSession
{
public:
    using PacketHandler = function<void(const LibNetworks::Core::Packet&)>;
    using ConnectHandler = function<void()>;
    using DisconnectHandler = function<void()>;

    virtual ~IBenchmarkSession() = default;

    virtual void SetPacketHandler(PacketHandler handler) = 0;
    virtual void SetConnectHandler(ConnectHandler handler) = 0;
    virtual void SetDisconnectHandler(DisconnectHandler handler) = 0;

    virtual void SendMessage(const uint16_t packetId, const google::protobuf::Message& rfMessage) = 0;
    virtual bool IsConnected() const = 0;
};

// IOCP 기반 벤치마크 세션
export class BenchmarkSessionIOCP : public LibNetworks::Sessions::OutboundSession, public IBenchmarkSession
{
public:
    BenchmarkSessionIOCP(const shared_ptr<LibNetworks::Core::Socket>& pSocket,
        unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
        unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer)
        : OutboundSession(pSocket, move(pReceiveBuffer), move(pSendBuffer))
    {
    }

    virtual ~BenchmarkSessionIOCP() = default;

    // IBenchmarkSession 구현
    void SetPacketHandler(PacketHandler handler) override { m_PacketHandler = move(handler); }
    void SetConnectHandler(ConnectHandler handler) override { m_ConnectHandler = move(handler); }
    void SetDisconnectHandler(DisconnectHandler handler) override { m_DisconnectHandler = move(handler); }

    bool IsConnected() const override { return m_Connected.load(); }

    void SendMessage(const uint16_t packetId, const google::protobuf::Message& rfMessage) override
    {
        // OutboundSession::SendMessage 호출
        LibNetworks::Sessions::OutboundSession::SendMessage(packetId, rfMessage);
    }

protected:
    void OnConnected() override
    {
        LibNetworks::Sessions::OutboundSession::OnConnected();
        m_Connected.store(true);
        if (m_ConnectHandler) m_ConnectHandler();
    }

    void OnDisconnected() override
    {
        LibNetworks::Sessions::OutboundSession::OnDisconnected();
        m_Connected.store(false);
        if (m_DisconnectHandler) m_DisconnectHandler();
    }

    void OnPacketReceived(const LibNetworks::Core::Packet& rfPacket) override
    {
        LibNetworks::Sessions::OutboundSession::OnPacketReceived(rfPacket);
        if (m_PacketHandler) m_PacketHandler(rfPacket);
    }

private:
    PacketHandler m_PacketHandler;
    ConnectHandler m_ConnectHandler;
    DisconnectHandler m_DisconnectHandler;
    atomic_bool m_Connected{ false };
};

// RIO 기반 벤치마크 세션
export class BenchmarkSessionRIO : public LibNetworks::Sessions::RIOSession, public IBenchmarkSession
{
public:
    BenchmarkSessionRIO(const shared_ptr<LibNetworks::Core::Socket>& pSocket,
        const LibNetworks::Core::RioBufferSlice& recvSlice,
        const LibNetworks::Core::RioBufferSlice& sendSlice,
        RIO_CQ completionQueue)
        : RIOSession(pSocket, recvSlice, sendSlice, completionQueue)
    {
    }

    virtual ~BenchmarkSessionRIO() = default;

    // IBenchmarkSession 구현
    void SetPacketHandler(PacketHandler handler) override { m_PacketHandler = move(handler); }
    void SetConnectHandler(ConnectHandler handler) override { m_ConnectHandler = move(handler); }
    void SetDisconnectHandler(DisconnectHandler handler) override { m_DisconnectHandler = move(handler); }

    bool IsConnected() const override { return RIOSession::IsConnected(); }

    void SendMessage(const uint16_t packetId, const google::protobuf::Message& rfMessage) override
    {
        // RIOSession::SendMessage 호출
        LibNetworks::Sessions::RIOSession::SendMessage(packetId, rfMessage);
    }

protected:
    void OnConnected() override
    {
        LibNetworks::Sessions::RIOSession::OnConnected();
        if (m_ConnectHandler) m_ConnectHandler();
    }

    void OnDisconnected() override
    {
        LibNetworks::Sessions::RIOSession::OnDisconnected();
        if (m_DisconnectHandler) m_DisconnectHandler();
    }

    void OnPacketReceived(const LibNetworks::Core::Packet& rfPacket) override
    {
        if (m_PacketHandler) m_PacketHandler(rfPacket);
    }

private:
    PacketHandler m_PacketHandler;
    ConnectHandler m_ConnectHandler;
    DisconnectHandler m_DisconnectHandler;
};

} // namespace FastPortBenchmark