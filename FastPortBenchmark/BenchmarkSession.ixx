module;

#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>

export module benchmark.session;

import networks.sessions.outbound_session;
import commons.buffers.ibuffer;
import networks.core.packet;

namespace FastPortBenchmark
{

export class BenchmarkSession : public LibNetworks::Sessions::OutboundSession
{
public:
    using PacketHandler = std::function<void(const LibNetworks::Core::Packet&)>;
    using ConnectHandler = std::function<void()>;
    using DisconnectHandler = std::function<void()>;

    BenchmarkSession() = delete;
    BenchmarkSession(const BenchmarkSession&) = delete;
    BenchmarkSession& operator=(const BenchmarkSession&) = delete;

    explicit BenchmarkSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer)
        : OutboundSession(pSocket, std::move(pReceiveBuffer), std::move(pSendBuffer))
    {
    }

    virtual ~BenchmarkSession() = default;

    void SetPacketHandler(PacketHandler handler) { m_PacketHandler = std::move(handler); }
    void SetConnectHandler(ConnectHandler handler) { m_ConnectHandler = std::move(handler); }
    void SetDisconnectHandler(DisconnectHandler handler) { m_DisconnectHandler = std::move(handler); }

    bool IsConnected() const { return m_Connected.load(); }

    void WaitForConnect(uint32_t timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_ConnectMutex);
        m_ConnectCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
            [this] { return m_Connected.load(); });
    }

protected:
    void OnConnected() override
    {
        __super::OnConnected();
        m_Connected.store(true);
        
        {
            std::lock_guard<std::mutex> lock(m_ConnectMutex);
            m_ConnectCv.notify_all();
        }

        if (m_ConnectHandler)
        {
            m_ConnectHandler();
        }
    }

    void OnDisconnected() override
    {
        __super::OnDisconnected();
        m_Connected.store(false);

        if (m_DisconnectHandler)
        {
            m_DisconnectHandler();
        }
    }

    void OnPacketReceived(const LibNetworks::Core::Packet& rfPacket) override
    {
        __super::OnPacketReceived(rfPacket);

        if (m_PacketHandler)
        {
            m_PacketHandler(rfPacket);
        }
    }

    void OnSent(size_t bytesSent) override
    {
        __super::OnSent(bytesSent);
    }

private:
    PacketHandler m_PacketHandler;
    ConnectHandler m_ConnectHandler;
    DisconnectHandler m_DisconnectHandler;

    std::atomic_bool m_Connected{false};
    std::mutex m_ConnectMutex;
    std::condition_variable m_ConnectCv;
};

} // namespace FastPortBenchmark
