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
#include <functional>
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

    TestRunner() = default;
    ~TestRunner() { Disconnect(); }

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

    std::shared_ptr<TestSession> ConnectOne(const char* ip, int port)
    {
        std::shared_ptr<TestSession> result;
        auto pMetrics = m_pMetrics;
        auto logCb = m_LogCallback;
        auto* pResult = &result;
        // Admin 콜백이 설정된 경우 첫 세션에 주입되도록 클로저로 캡처.
        const bool isFirst = m_Sessions.empty();
        auto adminSummaryCb = isFirst ? m_AdminSummaryCb : TestSession::AdminSummaryCallback{};
        auto adminListCb    = isFirst ? m_AdminSessionListCb : TestSession::AdminSessionListCallback{};

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

        auto pConnector = LibNetworks::Core::IOSocketConnector::Create(
            m_pIOService, pOnCreateSession, std::string(ip), static_cast<unsigned short>(port));

        if (!pConnector) return nullptr;

        m_Connectors.push_back(pConnector);
        return result;
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
};
