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
#include <vector>

// .pb.h 는 인터페이스 GMF 에서 include 하지 않는다. IFC 에 protobuf 정의가 실리면 MSVC 14.51.36231 이
// importer 쪽에서 C1116 ICE / C2504 를 낸다 (LibNetworks/INetworkSession.ixx 주석 참고).
// 메시지를 생성/파싱하는 멤버는 TestSession.cpp 에 구현하고, Admin 응답은 아래 plain struct 로 변환해 전달.

export module test_client.test_session;

import networks.sessions.outbound_session;
import networks.core.socket;
import networks.core.packet;
import commons.buffers.ibuffer;
import test_client.metrics_collector;

// Design Ref: server-status §5.2 — Admin 응답 스냅샷 (protobuf 비의존).
// AdminStatusSummaryResponse / AdminSessionListResponse 에서 UI 가 쓰는 필드만 복사.
export enum class AdminServerMode
{
    Unknown,
    IOCP,
    RIO,
};

export struct AdminSummarySnapshot
{
    AdminServerMode ServerMode = AdminServerMode::Unknown;
    uint64_t ServerUptimeMs = 0;
    uint32_t ActiveSessionCount = 0;
    uint64_t TotalRxBytes = 0;
    uint64_t TotalTxBytes = 0;
    uint64_t IdleDisconnectCount = 0;
    uint64_t ProcessMemoryBytes = 0;
    double ProcessCpuPercent = 0.0;
    uint64_t ServerTimestampMs = 0;
};

export struct AdminSessionInfoSnapshot
{
    uint64_t SessionId = 0;
    int64_t LastRecvMs = 0;
    uint64_t RxBytes = 0;
    uint64_t TxBytes = 0;
};

export struct AdminSessionListSnapshot
{
    uint32_t Total = 0;
    uint32_t Offset = 0;
    std::vector<AdminSessionInfoSnapshot> Sessions;
};

export class TestSession : public LibNetworks::Sessions::OutboundSession
{
public:
    using LogCallback = std::function<void(const char*)>;
    using ConnectedCallback = std::function<void(const std::shared_ptr<TestSession>&)>;
    using ActivatedCallback = std::function<void(const std::shared_ptr<TestSession>&)>;
    using DisconnectedCallback = std::function<void(uint64_t)>;

    TestSession(const std::shared_ptr<LibNetworks::Core::Socket>& pSocket,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
        std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer)
        : OutboundSession(pSocket, std::move(pReceiveBuffer), std::move(pSendBuffer))
    {
    }

    virtual ~TestSession() = default;

    void SetMetrics(MetricsCollector* pMetrics) { m_pMetrics = pMetrics; }
    void SetLogCallback(LogCallback cb) { m_LogCallback = std::move(cb); }
    void SetConnectedCallback(ConnectedCallback cb) { m_ConnectedCb = std::move(cb); }
    void SetActivatedCallback(ActivatedCallback cb) { m_ActivatedCb = std::move(cb); }
    void SetDisconnectedCallback(DisconnectedCallback cb) { m_DisconnectedCb = std::move(cb); }

    // Design Ref: server-status §5.2 — Admin 응답 콜백. AdminPanel 이 등록.
    using AdminSummaryCallback = std::function<void(const AdminSummarySnapshot&)>;
    using AdminSessionListCallback = std::function<void(const AdminSessionListSnapshot&)>;
    void SetAdminSummaryCallback(AdminSummaryCallback cb) { m_AdminSummaryCb = std::move(cb); }
    void SetAdminSessionListCallback(AdminSessionListCallback cb) { m_AdminSessionListCb = std::move(cb); }

    // Admin 요청 송신 — 서버의 0x8001/0x8003 엔드포인트로. (구현: TestSession.cpp)
    void SendAdminSummaryRequest();
    void SendAdminSessionListRequest(uint32_t offset, uint32_t limit);

    void SendEcho(const char* message);

    // Design Ref: iosession-lifetime-race §5.2 — Stress echo payload.
    // Ping-pong 에서 사용할 payload 를 세션별로 지정. 비어있으면 "echo" 기본 사용.
    void SetEchoPayload(std::string payload) { m_EchoPayload = std::move(payload); }

    uint64_t GetEchoCount() const { return m_EchoCount.load(std::memory_order_relaxed); }
    // 송신 시도 누적. SendEcho() 호출 시마다 +1. Stress Scenario B 에서 Packets Sent 표시용.
    uint64_t GetSendEchoCount() const { return m_SendEchoCount.load(std::memory_order_relaxed); }
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
        m_IsConnected.store(true, std::memory_order_release);
        auto self = std::static_pointer_cast<TestSession>(shared_from_this());
        __super::OnConnected();
        if (m_ConnectedCb)
        {
            m_ConnectedCb(self);
        }
        if (m_ActivatedCb)
        {
            m_ActivatedCb(self);
        }
        if (m_pMetrics) m_pMetrics->RecordConnection(true);
        Log("Connected to server");
    }

    void OnDisconnected() override
    {
        [[maybe_unused]] auto keepAlive = std::static_pointer_cast<TestSession>(shared_from_this());
        const auto sessionId = GetSessionId();
        auto disconnectedCb = m_DisconnectedCb;
        auto logCb = m_LogCallback;
        auto* pMetrics = m_pMetrics;

        m_IsConnected.store(false, std::memory_order_release);
        __super::OnDisconnected();
        if (pMetrics)
        {
            pMetrics->RecordConnection(false);
        }
        if (logCb)
        {
            logCb("Disconnected from server");
        }
        if (disconnectedCb)
        {
            disconnectedCb(sessionId);
        }
    }

    // (구현: TestSession.cpp)
    void OnPacketReceived(const LibNetworks::Core::Packet& rfPacket) override;

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
    std::atomic<uint64_t> m_SendEchoCount = 0;
    std::atomic<int> m_RemainingEchos = 0;
    std::atomic<bool> m_IsConnected = false;

    // Design Ref: iosession-lifetime-race §5.2 — Custom payload (default "echo").
    std::string m_EchoPayload;

    AdminSummaryCallback     m_AdminSummaryCb;
    AdminSessionListCallback m_AdminSessionListCb;
    ConnectedCallback        m_ConnectedCb;
    ActivatedCallback        m_ActivatedCb;
    DisconnectedCallback     m_DisconnectedCb;
};
