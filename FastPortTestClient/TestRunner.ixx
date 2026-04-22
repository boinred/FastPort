// Design Ref: §3.5 — TestRunner (connection + echo/flood/scale tests)
// Plan SC: SC1 (연결), SC3 (스케일 테스트)
module;

#include <WinSock2.h>
#include <MSWSock.h>
#include <memory>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <random>
#include <limits>
#include <algorithm>
#include <functional>
#include <utility>
#include <spdlog/spdlog.h>

export module test_client.test_runner;

import networks.services.io_service;
import networks.core.io_socket_connector;
import networks.core.socket;
import networks.sessions.outbound_session;
import commons.buffers.circle_buffer_queue;
import commons.event_listener;
import test_client.metrics_collector;
import test_client.test_session;

export class TestRunner
{
public:
    using LogCallback = std::function<void(const char*)>;

    // Design Ref: iosession-lifetime-race §5 — Stress reproducer (v0.2 — 3 시나리오).
    // Plan v0.2 FR-10 — Scenario A (Churn 10k×5min) + B (Burst 1M×2) + C (Combined 1k×100pps×5min).
    enum class StressScenario : int
    {
        Churn    = 0,   // A: 10k 동시 + 100 churn/s × duration
        Burst    = 1,   // B: N concurrent × 1M packet × 2 round
        Combined = 2,   // C: 1k conn × ~100 pps × duration
    };

    struct StressConfig
    {
        StressScenario scenario = StressScenario::Churn;

        // 공통
        int  durationSec  = 300;          // A, C 에서 사용
        bool enableEcho   = true;
        int  payloadBytes = 4;            // 4~8192

        // Scenario A — Churn
        int targetConns     = 10000;
        int churnRatePerSec = 100;

        // Scenario B — Burst (Plan v0.2 — 1M × 2 round)
        int burstConcurrentSessions = 1;          // 1~8
        int burstPacketCount        = 1000000;    // round 당 예상 수신 패킷 (집계 목표)
        int burstRoundCount         = 2;

        // Scenario C — Combined
        int combinedConnCount     = 1000;
        int combinedPpsPerSession = 100;   // 참조값 — 실제 페이싱은 echo loop 자연 속도
    };

    // Design Ref: iosession-lifetime-race §5.4 Page UI Checklist — stat fields.
    struct StressStats
    {
        std::atomic<std::uint64_t> connectAttempts  { 0 };
        std::atomic<std::uint64_t> connectFailures  { 0 };
        std::atomic<std::uint64_t> totalAccepted    { 0 };
        std::atomic<std::uint64_t> churned          { 0 };
        std::atomic<int>           active           { 0 };
        std::atomic<int>           elapsedSec       { 0 };
        std::atomic<bool>          running          { false };
        std::atomic<bool>          crashSuspected   { false };

        // v0.2 — Scenario B/C 용 진행 스냅샷. metrics 기반 파생값.
        std::atomic<std::uint64_t> packetsReceived  { 0 };
        std::atomic<int>           currentRound     { 0 };   // Scenario B: 1..roundCount
    };

    TestRunner() = default;
    ~TestRunner() { Disconnect(); StopStressMode(); }

    void SetMetrics(MetricsCollector* pMetrics) { m_pMetrics = pMetrics; }
    void SetLogCallback(LogCallback cb) { m_LogCallback = std::move(cb); }

    bool Connect(const char* ip, int port)
    {
        if (m_bConnected) return true;

        if (!EnsureIOService()) return false;

        auto pSession = ConnectOne(ip, port);
        if (!pSession) return false;

        m_Sessions.push_back(pSession);
        m_bConnected = true;
        return true;
    }

    void Disconnect()
    {
        // 1. 진행 중인 테스트 중단
        m_bFloodRunning = false;

        // 2. 커넥터 해제 (소켓 닫기 → IO 완료 통지 중단)
        for (auto& c : m_Connectors)
        {
            if (c) c->DisConnect();
        }
        m_Connectors.clear();

        // 3. IO 서비스 중지 (워커 스레드 종료 대기)
        if (m_pIOService)
        {
            m_pIOService->Stop();
            m_pIOService.reset();
        }

        // 4. 워커 스레드 종료 후 세션 해제 (use-after-free 방지)
        m_Sessions.clear();
        m_bConnected = false;
    }

    bool IsConnected() const { return m_bConnected; }

    // Design Ref: §3.5 — Echo test (에코 N회)
    void RunEchoTest(int count)
    {
        for (auto& pSession : m_Sessions)
        {
            if (pSession) pSession->StartEchoLoop(count);
        }
    }

    // Design Ref: §3.5 — Flood test (N초간 최대 속도)
    void RunFloodTest(int durationSec)
    {
        m_bFloodRunning = true;
        m_FloodThread = std::thread([this, durationSec]()
        {
            auto endTime = std::chrono::steady_clock::now() + std::chrono::seconds(durationSec);
            while (m_bFloodRunning && std::chrono::steady_clock::now() < endTime)
            {
                for (auto& pSession : m_Sessions)
                {
                    if (pSession) pSession->SendEcho("flood");
                }
            }
            m_bFloodRunning = false;
        });
        m_FloodThread.detach();
    }

    void StopFlood() { m_bFloodRunning = false; }
    bool IsFloodRunning() const { return m_bFloodRunning; }

    // Design Ref: §3.5 — Generic test state (covers echo + flood)
    bool IsTestRunning() const
    {
        if (m_bFloodRunning) return true;
        for (auto& pSession : m_Sessions)
        {
            if (pSession && pSession->GetEchoCount() > 0) return true;
        }
        return false;
    }

    void StopTest()
    {
        m_bFloodRunning = false;
        // Echo는 세션 단위로 남은 횟수를 소진하므로 별도 중단 불필요
    }

    bool ConnectScale(const char* ip, int port, int count)
    {
        if (!EnsureIOService()) return false;

        int success = 0;
        for (int i = 0; i < count; ++i)
        {
            auto pSession = ConnectOne(ip, port);
            if (pSession)
            {
                m_Sessions.push_back(pSession);
                ++success;
            }
        }

        m_bConnected = (success > 0) || m_bConnected;
        return success > 0;
    }

    int GetConnectionCount() const { return static_cast<int>(m_Connectors.size()); }

    // 세션 접근 (에코 테스트 등에서 사용)
    const std::vector<std::shared_ptr<TestSession>>& GetSessions() const { return m_Sessions; }

    // Design Ref: server-status §5.2 — Admin 요청은 첫 세션(주 연결)을 통해 송신.
    // Scale 테스트로 늘어난 세션들은 제외 — admin 은 UI 가 하나의 채널로 폴링.
    bool SendAdminSummaryRequest()
    {
        if (m_Sessions.empty() || !m_Sessions.front()) return false;
        m_Sessions.front()->SendAdminSummaryRequest();
        return true;
    }

    bool SendAdminSessionListRequest(uint32_t offset, uint32_t limit)
    {
        if (m_Sessions.empty() || !m_Sessions.front()) return false;
        m_Sessions.front()->SendAdminSessionListRequest(offset, limit);
        return true;
    }

    void SetAdminSummaryCallback(TestSession::AdminSummaryCallback cb)
    {
        m_AdminSummaryCb = std::move(cb);
        if (!m_Sessions.empty() && m_Sessions.front())
        {
            m_Sessions.front()->SetAdminSummaryCallback(m_AdminSummaryCb);
        }
    }

    void SetAdminSessionListCallback(TestSession::AdminSessionListCallback cb)
    {
        m_AdminSessionListCb = std::move(cb);
        if (!m_Sessions.empty() && m_Sessions.front())
        {
            m_Sessions.front()->SetAdminSessionListCallback(m_AdminSessionListCb);
        }
    }

    // -------------------------------------------------------------------------
    // Design Ref: iosession-lifetime-race §5.2 — Stress mode (lifetime race
    // reproducer). Target: targetConns 수립 후 durationSec 동안 초당 churnRatePerSec
    // 개 세션을 disconnect + reconnect 반복. UAF 재현 및 패치 검증용.
    // -------------------------------------------------------------------------
    bool StartStressMode(const char* ip, int port, const StressConfig& cfg)
    {
        if (m_StressStats.running.load(std::memory_order_acquire)) return false;
        if (!EnsureIOService()) return false;

        // Stats reset.
        m_StressStats.connectAttempts.store(0, std::memory_order_relaxed);
        m_StressStats.connectFailures.store(0, std::memory_order_relaxed);
        m_StressStats.totalAccepted.store(0, std::memory_order_relaxed);
        m_StressStats.churned.store(0, std::memory_order_relaxed);
        m_StressStats.active.store(0, std::memory_order_relaxed);
        m_StressStats.elapsedSec.store(0, std::memory_order_relaxed);
        m_StressStats.crashSuspected.store(false, std::memory_order_relaxed);
        m_StressStats.packetsReceived.store(0, std::memory_order_relaxed);
        m_StressStats.currentRound.store(0, std::memory_order_relaxed);
        m_StressStats.running.store(true, std::memory_order_release);

        std::string ipCopy(ip);
        m_StressThread = std::thread(&TestRunner::StressMain, this, ipCopy, port, cfg);
        return true;
    }

    void StopStressMode()
    {
        m_StressStats.running.store(false, std::memory_order_release);
        if (m_StressThread.joinable()) m_StressThread.join();

        // 남아있는 stress 연결 정리.
        std::vector<std::shared_ptr<LibNetworks::Core::IOSocketConnector>> toDrop;
        {
            std::lock_guard<std::mutex> lock(m_StressMutex);
            toDrop.swap(m_StressConnectors);
            m_StressSessions.clear();
            m_StressStats.active.store(0, std::memory_order_relaxed);
        }
        for (auto& c : toDrop) { if (c) c->DisConnect(); }
    }

    const StressStats& GetStressStats() const { return m_StressStats; }
    bool IsStressRunning() const { return m_StressStats.running.load(std::memory_order_acquire); }

private:
    bool EnsureIOService()
    {
        if (!m_pIOService)
        {
            m_pIOService = std::make_shared<LibNetworks::Services::IOService>();
            m_pIOService->Start(std::thread::hardware_concurrency());
        }
        return m_pIOService != nullptr;
    }

    // 공용 저수준 헬퍼: connector 를 만들고 (session, connector) 쌍 반환.
    // push 는 호출자 책임. Connect/ConnectScale 은 m_Connectors/m_Sessions 에,
    // Stress 는 m_StressConnectors/m_StressSessions 에 각각 저장.
    struct CreateResult
    {
        std::shared_ptr<TestSession> session;
        std::shared_ptr<LibNetworks::Core::IOSocketConnector> connector;
    };

    CreateResult CreateConnection(const char* ip, int port, bool injectAdminCallbacks)
    {
        CreateResult out;
        auto pMetrics = m_pMetrics;
        auto logCb = m_LogCallback;
        auto* pResult = &out.session;

        auto adminSummaryCb = injectAdminCallbacks ? m_AdminSummaryCb : TestSession::AdminSummaryCallback{};
        auto adminListCb    = injectAdminCallbacks ? m_AdminSessionListCb : TestSession::AdminSessionListCallback{};

        auto pOnCreateSession = [pMetrics, logCb, pResult, adminSummaryCb, adminListCb](const std::shared_ptr<LibNetworks::Core::Socket>& pSocket)
            -> std::shared_ptr<LibNetworks::Sessions::OutboundSession>
            {
                const size_t bufferSize = 8 * 1024;
                auto pRecv = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(bufferSize);
                auto pSend = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(bufferSize);
                auto pSession = std::make_shared<TestSession>(pSocket, std::move(pRecv), std::move(pSend));
                pSession->SetMetrics(pMetrics);
                pSession->SetLogCallback(logCb);
                if (adminSummaryCb) pSession->SetAdminSummaryCallback(adminSummaryCb);
                if (adminListCb)    pSession->SetAdminSessionListCallback(adminListCb);
                *pResult = pSession;
                return pSession;
            };

        out.connector = LibNetworks::Core::IOSocketConnector::Create(
            m_pIOService, pOnCreateSession, std::string(ip), static_cast<unsigned short>(port));

        return out;
    }

    std::shared_ptr<TestSession> ConnectOne(const char* ip, int port)
    {
        const bool isFirst = m_Sessions.empty();
        auto res = CreateConnection(ip, port, isFirst);
        if (!res.connector) return nullptr;
        m_Connectors.push_back(res.connector);
        return res.session;
    }

    // Design Ref: iosession-lifetime-race §5 — Stress scenario dispatcher (v0.2).
    // A/B/C 시나리오 별 main loop 를 분기 호출. running 플래그는 여기서 일괄 해제.
    void StressMain(std::string ip, int port, StressConfig cfg)
    {
        switch (cfg.scenario)
        {
        case StressScenario::Churn:    StressMain_Churn(ip, port, cfg);    break;
        case StressScenario::Burst:    StressMain_Burst(ip, port, cfg);    break;
        case StressScenario::Combined: StressMain_Combined(ip, port, cfg); break;
        }
        m_StressStats.running.store(false, std::memory_order_release);
    }

    // Design Ref: iosession-lifetime-race §5.2 Scenario A — Churn.
    // Phase 1: target 연결 수 빌드업 (100/tick 페이싱). Phase 2: churn 반복.
    void StressMain_Churn(std::string ip, int port, StressConfig cfg)
    {
        using namespace std::chrono;
        const auto startTime = steady_clock::now();

        // Echo payload 생성 — 세션 간 공유 (payload 내용은 중요 x, 크기만 중요).
        // 빈 문자열이면 TestSession 이 기본 "echo" 사용.
        const std::string payload =
            (cfg.enableEcho && cfg.payloadBytes > 0)
                ? BuildPayload(cfg.payloadBytes)
                : std::string{};
        m_StressPayload = payload;        // 새 연결 시 재사용
        m_StressEnableEcho = cfg.enableEcho;

        // Phase 1: Build-up.
        const int buildBatch = 100;   // 한 번에 붙일 개수 (kernel 부하 완화)
        const auto batchInterval = milliseconds(10);
        int built = 0;
        while (built < cfg.targetConns && m_StressStats.running.load(std::memory_order_acquire))
        {
            const int n = (std::min)(buildBatch, cfg.targetConns - built);
            for (int i = 0; i < n; ++i) { TryStressConnect(ip.c_str(), port); }
            built += n;

            m_StressStats.elapsedSec.store(
                static_cast<int>(duration_cast<seconds>(steady_clock::now() - startTime).count()),
                std::memory_order_relaxed);

            // crash suspect: 시작 단계에서 실패율 50% 초과면 서버 미기동 의심.
            const auto attempts = m_StressStats.connectAttempts.load(std::memory_order_relaxed);
            const auto failures = m_StressStats.connectFailures.load(std::memory_order_relaxed);
            if (attempts > 100 && failures * 2 > attempts)
            {
                m_StressStats.crashSuspected.store(true, std::memory_order_relaxed);
            }

            std::this_thread::sleep_for(batchInterval);
        }

        // Design Ref: iosession-lifetime-race §5.2 — Build-up 완료 시점에 모든 세션
        // ping-pong 시작. Race window 극대화.
        if (cfg.enableEcho)
        {
            StartEchoOnAllStressSessions();
        }

        // Phase 2: Churn loop.
        const auto churnEnd = startTime + seconds(cfg.durationSec);
        const auto tickInterval = milliseconds(10);   // 100Hz
        const int churnPerTick = (std::max)(1, cfg.churnRatePerSec / 100);
        auto nextTick = steady_clock::now();

        while (m_StressStats.running.load(std::memory_order_acquire)
            && steady_clock::now() < churnEnd)
        {
            nextTick += tickInterval;
            std::this_thread::sleep_until(nextTick);

            for (int i = 0; i < churnPerTick; ++i) { StressChurnOne(ip.c_str(), port); }

            m_StressStats.elapsedSec.store(
                static_cast<int>(duration_cast<seconds>(steady_clock::now() - startTime).count()),
                std::memory_order_relaxed);
        }

        // Phase 3: 자연 종료 (연결 유지 — 사용자가 StopStressMode 호출 시 cleanup).
        // running flag 는 dispatcher (StressMain) 에서 일괄 해제.
    }

    // Design Ref: iosession-lifetime-race §5.2 Scenario B — Burst 1M × N round.
    // burstConcurrentSessions 개 세션을 유지하며, 각 라운드마다 echo loop 시작.
    // 라운드 완료 판정: metrics 의 누적 packet 수가 round target 에 도달하거나,
    // round timeout (60 초) 도달 시. UAF 관찰이 목표이므로 정확한 pacing 불요.
    void StressMain_Burst(std::string ip, int port, StressConfig cfg)
    {
        using namespace std::chrono;
        const auto startTime = steady_clock::now();

        const int sessions = (std::max)(1, (std::min)(cfg.burstConcurrentSessions, 8));
        const int rounds   = (std::max)(1, cfg.burstRoundCount);
        const int perRound = (std::max)(1, cfg.burstPacketCount);

        // Echo payload 준비.
        const std::string payload =
            (cfg.enableEcho && cfg.payloadBytes > 0)
                ? BuildPayload(cfg.payloadBytes)
                : std::string{};
        m_StressPayload = payload;
        m_StressEnableEcho = cfg.enableEcho;

        // Phase 1: N sessions 빌드업.
        for (int i = 0; i < sessions && m_StressStats.running.load(std::memory_order_acquire); ++i)
        {
            TryStressConnect(ip.c_str(), port);
        }

        // round 별 시작 시 metrics baseline 기록.
        const std::uint64_t metricsBaseline = SnapshotReceivedPackets();

        for (int r = 1; r <= rounds && m_StressStats.running.load(std::memory_order_acquire); ++r)
        {
            m_StressStats.currentRound.store(r, std::memory_order_relaxed);

            // 각 세션에 이번 라운드 분량 echo loop 지시.
            std::vector<std::shared_ptr<TestSession>> snapshot;
            {
                std::lock_guard<std::mutex> lock(m_StressMutex);
                snapshot = m_StressSessions;
            }
            for (auto& s : snapshot)
            {
                if (!s) continue;
                if (!payload.empty()) s->SetEchoPayload(payload);
                s->StartEchoLoop(perRound);
            }

            // 라운드 완료 대기: round target 도달 or 60s 타임아웃 (UAF reproducer 상,
            // 장시간 트래픽이 더 중요하므로 완료 못 해도 타임아웃으로 넘어감).
            const std::uint64_t roundTarget =
                metricsBaseline + static_cast<std::uint64_t>(r) * static_cast<std::uint64_t>(perRound);
            const auto roundTimeout = steady_clock::now() + seconds(60);

            while (m_StressStats.running.load(std::memory_order_acquire)
                && steady_clock::now() < roundTimeout)
            {
                const std::uint64_t received = SnapshotReceivedPackets();
                m_StressStats.packetsReceived.store(received - metricsBaseline, std::memory_order_relaxed);
                m_StressStats.elapsedSec.store(
                    static_cast<int>(duration_cast<seconds>(steady_clock::now() - startTime).count()),
                    std::memory_order_relaxed);

                if (received >= roundTarget) break;
                std::this_thread::sleep_for(milliseconds(100));
            }
        }

        // 자연 종료 — 사용자가 StopStressMode 호출 시 cleanup. running flag 는 dispatcher 에서 해제.
    }

    // Design Ref: iosession-lifetime-race §5.2 Scenario C — Combined.
    // 1k conn 빌드업 + 세션별 echo loop + durationSec 동안 유지.
    // 정확한 PPS 페이싱은 목표 아님 — 세션 수 × echo 자연속도로 대량 트래픽 유지.
    void StressMain_Combined(std::string ip, int port, StressConfig cfg)
    {
        using namespace std::chrono;
        const auto startTime = steady_clock::now();

        const int targetConns = (std::max)(1, (std::min)(cfg.combinedConnCount, 50000));

        const std::string payload =
            (cfg.enableEcho && cfg.payloadBytes > 0)
                ? BuildPayload(cfg.payloadBytes)
                : std::string{};
        m_StressPayload = payload;
        m_StressEnableEcho = cfg.enableEcho;

        // Phase 1: Build-up (100 / 10ms 페이싱).
        const int buildBatch = 100;
        const auto batchInterval = milliseconds(10);
        int built = 0;
        while (built < targetConns && m_StressStats.running.load(std::memory_order_acquire))
        {
            const int n = (std::min)(buildBatch, targetConns - built);
            for (int i = 0; i < n; ++i) { TryStressConnect(ip.c_str(), port); }
            built += n;

            m_StressStats.elapsedSec.store(
                static_cast<int>(duration_cast<seconds>(steady_clock::now() - startTime).count()),
                std::memory_order_relaxed);

            const auto attempts = m_StressStats.connectAttempts.load(std::memory_order_relaxed);
            const auto failures = m_StressStats.connectFailures.load(std::memory_order_relaxed);
            if (attempts > 100 && failures * 2 > attempts)
            {
                m_StressStats.crashSuspected.store(true, std::memory_order_relaxed);
            }

            std::this_thread::sleep_for(batchInterval);
        }

        if (cfg.enableEcho) { StartEchoOnAllStressSessions(); }

        // Phase 2: 유지 단계 — durationSec 동안 살아있는 세션 유지 + metrics 추적.
        const auto endTime = startTime + seconds(cfg.durationSec);
        const std::uint64_t metricsBaseline = SnapshotReceivedPackets();
        while (m_StressStats.running.load(std::memory_order_acquire)
            && steady_clock::now() < endTime)
        {
            const std::uint64_t received = SnapshotReceivedPackets();
            m_StressStats.packetsReceived.store(received - metricsBaseline, std::memory_order_relaxed);
            m_StressStats.elapsedSec.store(
                static_cast<int>(duration_cast<seconds>(steady_clock::now() - startTime).count()),
                std::memory_order_relaxed);
            std::this_thread::sleep_for(milliseconds(200));
        }
    }

    // MetricsCollector 가 설정된 경우에만 수신 패킷 수 반환, 아니면 0.
    std::uint64_t SnapshotReceivedPackets()
    {
        if (!m_pMetrics) return 0;
        return m_pMetrics->GetTotalMessages();
    }

    // 단일 연결 시도 (stress). 성공/실패 counter 업데이트.
    // Build-up 이 끝난 이후 (즉 m_StressEnableEcho 이 true 이고 이미 ping-pong 단계)
    // 에 새로 연결된 세션 (churn reconnect) 도 바로 echo 시작.
    void TryStressConnect(const char* ip, int port)
    {
        m_StressStats.connectAttempts.fetch_add(1, std::memory_order_relaxed);
        auto res = CreateConnection(ip, port, /*injectAdminCallbacks=*/false);
        if (!res.connector)
        {
            m_StressStats.connectFailures.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Echo payload 주입. Build-up 단계에서는 플래그만 설정 (실제 echo 시작은
        // StressMain build-up 완료 후 일괄). Churn reconnect 는 즉시 echo 시작.
        const bool startImmediately = m_StressStats.active.load(std::memory_order_relaxed) > 0
                                    && m_StressEnableEcho;
        if (res.session && !m_StressPayload.empty())
        {
            res.session->SetEchoPayload(m_StressPayload);
        }
        if (startImmediately && res.session)
        {
            // 이미 build-up 이후 — churn 으로 새로 연결된 세션이면 바로 ping-pong 시작.
            res.session->StartEchoLoop((std::numeric_limits<int>::max)());
        }

        std::lock_guard<std::mutex> lock(m_StressMutex);
        m_StressConnectors.push_back(res.connector);
        m_StressSessions.push_back(res.session);
        m_StressStats.totalAccepted.fetch_add(1, std::memory_order_relaxed);
        m_StressStats.active.fetch_add(1, std::memory_order_relaxed);
    }

    // Build-up 완료 후 전체 세션에 echo loop 시작.
    void StartEchoOnAllStressSessions()
    {
        std::vector<std::shared_ptr<TestSession>> snapshot;
        {
            std::lock_guard<std::mutex> lock(m_StressMutex);
            snapshot = m_StressSessions;
        }
        for (auto& s : snapshot)
        {
            if (!s) continue;
            if (!m_StressPayload.empty()) { s->SetEchoPayload(m_StressPayload); }
            s->StartEchoLoop((std::numeric_limits<int>::max)());
        }
    }

    // payload 생성 — "abcdefghijklmnopqrstuvwxyz" 를 bytes 만큼 반복.
    // 서버가 단순 echo 하므로 내용은 중요하지 않고 크기만 중요.
    static std::string BuildPayload(int bytes)
    {
        if (bytes < 1) return {};
        static constexpr const char* kBase = "abcdefghijklmnopqrstuvwxyz0123456789";
        const int baseLen = static_cast<int>(std::char_traits<char>::length(kBase));
        std::string out;
        out.reserve(bytes);
        while (static_cast<int>(out.size()) < bytes)
        {
            const int remain = bytes - static_cast<int>(out.size());
            out.append(kBase, (std::min)(baseLen, remain));
        }
        return out;
    }

    // Design Ref: iosession-lifetime-race §2.2 Data Flow — disconnect + reconnect.
    // 이 경로가 UAF race 를 유발하는 "세션 수명 vs worker dispatch" 타이밍을 최대화한다.
    void StressChurnOne(const char* ip, int port)
    {
        std::shared_ptr<LibNetworks::Core::IOSocketConnector> victim;
        {
            std::lock_guard<std::mutex> lock(m_StressMutex);
            if (m_StressConnectors.empty()) return;
            std::uniform_int_distribution<size_t> dist(0, m_StressConnectors.size() - 1);
            const size_t idx = dist(m_StressRng);
            victim = m_StressConnectors[idx];
            m_StressConnectors.erase(m_StressConnectors.begin() + idx);
            if (idx < m_StressSessions.size())
            {
                m_StressSessions.erase(m_StressSessions.begin() + idx);
            }
            m_StressStats.active.fetch_sub(1, std::memory_order_relaxed);
        }
        if (victim) victim->DisConnect();

        TryStressConnect(ip, port);
        m_StressStats.churned.fetch_add(1, std::memory_order_relaxed);
    }

    MetricsCollector* m_pMetrics = nullptr;
    LogCallback m_LogCallback;
    std::shared_ptr<LibNetworks::Services::IOService> m_pIOService;
    std::vector<std::shared_ptr<LibNetworks::Core::IOSocketConnector>> m_Connectors;
    std::vector<std::shared_ptr<TestSession>> m_Sessions;
    bool m_bConnected = false;
    std::atomic<bool> m_bFloodRunning = false;
    std::thread m_FloodThread;

    // Admin 콜백 보존 — Connect 이후 첫 세션 생성 시 전달.
    TestSession::AdminSummaryCallback     m_AdminSummaryCb;
    TestSession::AdminSessionListCallback m_AdminSessionListCb;

    // Stress mode 전용 상태 (mutex 로 churn 스레드 ↔ cleanup 호출자 보호).
    StressStats                                                        m_StressStats;
    std::thread                                                        m_StressThread;
    std::mutex                                                         m_StressMutex;
    std::vector<std::shared_ptr<LibNetworks::Core::IOSocketConnector>> m_StressConnectors;
    std::vector<std::shared_ptr<TestSession>>                          m_StressSessions;
    std::mt19937                                                       m_StressRng { std::random_device{}() };

    // Echo payload (공유). Build-up 시 결정, churn reconnect 에도 재사용.
    std::string                                                        m_StressPayload;
    bool                                                               m_StressEnableEcho { true };
};
