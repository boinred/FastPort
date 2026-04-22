// Design Ref: §3.3 — TestSession (OutboundSession + RTT measurement)
// Plan SC: SC1 (서버 연결), SC2 (RTT 측정)
module;

#include <WinSock2.h>
#include <MSWSock.h>
#include <memory>
#include <string>
#include <utility>
#include <chrono>
#include <atomic>
#include <functional>
#include <spdlog/spdlog.h>

#include "Protocols/Commons.pb.h"
#include "Protocols/Tests.pb.h"
#include "Protocols/Admin.pb.h"

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

    // Design Ref: server-status §5.2 — Admin 응답 콜백. AdminPanel 이 등록.
    using AdminSummaryCallback = std::function<void(const ::fastport::protocols::admin::AdminStatusSummaryResponse&)>;
    using AdminSessionListCallback = std::function<void(const ::fastport::protocols::admin::AdminSessionListResponse&)>;
    void SetAdminSummaryCallback(AdminSummaryCallback cb) { m_AdminSummaryCb = std::move(cb); }
    void SetAdminSessionListCallback(AdminSessionListCallback cb) { m_AdminSessionListCb = std::move(cb); }

    // Admin 요청 송신 — 서버의 0x8001/0x8003 엔드포인트로.
    void SendAdminSummaryRequest()
    {
        ::fastport::protocols::admin::AdminStatusSummaryRequest request;
        auto pHeader = request.mutable_header();
        pHeader->set_request_id(m_NextRequestId++);
        pHeader->set_timestamp_ms(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()));
        SendMessage(0x8001, request);
    }

    void SendAdminSessionListRequest(uint32_t offset, uint32_t limit)
    {
        ::fastport::protocols::admin::AdminSessionListRequest request;
        auto pHeader = request.mutable_header();
        pHeader->set_request_id(m_NextRequestId++);
        pHeader->set_timestamp_ms(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count()));
        request.set_offset(offset);
        request.set_limit(limit);
        SendMessage(0x8003, request);
    }

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

    // Design Ref: iosession-lifetime-race §5.2 — Stress echo payload.
    // Ping-pong 에서 사용할 payload 를 세션별로 지정. 비어있으면 "echo" 기본 사용.
    void SetEchoPayload(std::string payload) { m_EchoPayload = std::move(payload); }

    uint64_t GetEchoCount() const { return m_EchoCount.load(std::memory_order_relaxed); }
    bool IsConnected() const noexcept { return m_IsConnected.load(std::memory_order_acquire); }
    bool TryStartEchoLoop(int count)
    {
        if (!IsConnected())
        {
            return false;
        }

        StartEchoLoop(count);
        return true;
    }

    void OnConnected() override
    {
        __super::OnConnected();
        m_IsConnected.store(true, std::memory_order_release);
        if (m_pMetrics) m_pMetrics->RecordConnection(true);
        Log("Connected to server");
    }

    void OnDisconnected() override
    {
        m_IsConnected.store(false, std::memory_order_release);
        __super::OnDisconnected();
        if (m_pMetrics) m_pMetrics->RecordConnection(false);
        Log("Disconnected from server");
    }

    void OnPacketReceived(const LibNetworks::Core::Packet& rfPacket) override
    {
        const auto packetId = rfPacket.GetPacketId();

        // Design Ref: server-status §5.2 — Admin 응답은 콜백으로 전달.
        if (packetId == 0x8002)
        {
            ::fastport::protocols::admin::AdminStatusSummaryResponse response;
            if (rfPacket.ParseMessage(response) && m_AdminSummaryCb)
            {
                m_AdminSummaryCb(response);
            }
            return;
        }
        if (packetId == 0x8004)
        {
            ::fastport::protocols::admin::AdminSessionListResponse response;
            if (rfPacket.ParseMessage(response) && m_AdminSessionListCb)
            {
                m_AdminSessionListCb(response);
            }
            return;
        }

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
            const char* payload = m_EchoPayload.empty() ? "echo" : m_EchoPayload.c_str();
            SendEcho(payload);
        }
    }

    void OnSent(size_t bytesSent) override
    {
        __super::OnSent(bytesSent);
    }

    void StartEchoLoop(int count)
    {
        m_RemainingEchos = count - 1; // 첫 번째는 바로 전송
        const char* payload = m_EchoPayload.empty() ? "echo" : m_EchoPayload.c_str();
        SendEcho(payload);
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
    std::atomic<bool> m_IsConnected = false;

    // Design Ref: iosession-lifetime-race §5.2 — Custom payload (default "echo").
    std::string m_EchoPayload;

    AdminSummaryCallback     m_AdminSummaryCb;
    AdminSessionListCallback m_AdminSessionListCb;
};
