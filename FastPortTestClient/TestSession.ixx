// Design Ref: §3.3 — TestSession (OutboundSession + RTT measurement)
// Plan SC: SC1 (서버 연결), SC2 (RTT 측정)
module;

#include <WinSock2.h>
#include <MSWSock.h>
#include <memory>
#include <chrono>
#include <atomic>
#include <functional>
#include <spdlog/spdlog.h>

#include "Protocols/Commons.pb.h"
#include "Protocols/Tests.pb.h"

export module test_client.test_session;

import networks.sessions.outbound_session;
import networks.core.socket;
import networks.core.packet;
import commons.buffers.ibuffer;
import commons.buffers.circle_buffer_queue;
import commons.logger;
import test_client.metrics_collector;

export class TestSession : public LibNetworks::Sessions::OutboundSession
{
public:
    using LogCallback = std::function<void(const char*)>;

    TestSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer)
        : OutboundSession(pSocket, std::move(pReceiveBuffer), std::move(pSendBuffer))
    {
    }

    virtual ~TestSession() = default;

    void SetMetrics(MetricsCollector* pMetrics) { m_pMetrics = pMetrics; }
    void SetLogCallback(LogCallback cb) { m_LogCallback = std::move(cb); }

    void SendEcho(const char* message)
    {
        m_LastSendTime = std::chrono::steady_clock::now();

        ::fastport::protocols::tests::EchoRequest request;
        auto pHeader = request.mutable_header();
        pHeader->set_request_id(m_NextRequestId++);
        pHeader->set_timestamp_ms(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                m_LastSendTime.time_since_epoch()).count()));
        request.set_data_str(message);

        SendMessage(::fastport::protocols::commons::ProtocolId::PROTOCOL_ID_TESTS, request);
    }

    uint64_t GetEchoCount() const { return m_EchoCount.load(std::memory_order_relaxed); }

    void OnConnected() override
    {
        __super::OnConnected();
        if (m_pMetrics) m_pMetrics->RecordConnection(true);
        Log("Connected to server");
    }

    void OnDisconnected() override
    {
        __super::OnDisconnected();
        if (m_pMetrics) m_pMetrics->RecordConnection(false);
        Log("Disconnected from server");
    }

    void OnPacketReceived(const LibNetworks::Core::Packet& rfPacket) override
    {
        auto recvTime = std::chrono::steady_clock::now();
        double rttMs = std::chrono::duration<double, std::milli>(recvTime - m_LastSendTime).count();

        m_EchoCount.fetch_add(1, std::memory_order_relaxed);

        if (m_pMetrics)
        {
            m_pMetrics->RecordLatency(rttMs);
            m_pMetrics->RecordMessage();
        }

        // 에코 테스트 잔여 횟수가 있으면 계속 전송
        if (m_RemainingEchos.load(std::memory_order_relaxed) > 0)
        {
            m_RemainingEchos.fetch_sub(1, std::memory_order_relaxed);
            SendEcho("echo");
        }
    }

    void OnSent(size_t bytesSent) override
    {
        __super::OnSent(bytesSent);
    }

    void StartEchoLoop(int count)
    {
        m_RemainingEchos = count - 1; // 첫 번째는 바로 전송
        SendEcho("echo");
    }

private:
    void Log(const char* msg)
    {
        if (m_LogCallback) m_LogCallback(msg);
    }

    MetricsCollector* m_pMetrics = nullptr;
    LogCallback m_LogCallback;
    std::chrono::steady_clock::time_point m_LastSendTime;
    uint32_t m_NextRequestId = 1;
    std::atomic<uint64_t> m_EchoCount = 0;
    std::atomic<int> m_RemainingEchos = 0;
};
