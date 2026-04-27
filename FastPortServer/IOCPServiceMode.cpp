module;

#include <windows.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>

module iocp_service_mode;

import std;
import commons.logger;
import commons.container;                 // SessionContainer ForEach
import commons.singleton;                  // SingleTon<SessionContainer>
import networks.sessions.inetwork_session;
import networks.sessions.inbound_session;
import networks.sessions.iidle_aware;     // SnapshotProvider target
import networks.sessions.isession_stats;  // server-status


// Design Ref: session-idle-timeout §4.4 / server-status §4.2 — 활성 세션 전역 컨테이너.
// IOCPInboundSession.cpp 와 동일 타입으로 같은 SingleTon 인스턴스 공유.
using SessionContainer = LibCommons::Container<
    uint64_t,
    std::shared_ptr<LibNetworks::Sessions::InboundSession>>;


// Design Ref: server-status §4.4 — AdminPacketHandler 전역 액세스 포인트.
// IOCPInboundSession::OnPacketReceived 에서 admin 패킷을 dispatch 할 때 사용.
// ServiceMode 가 생성·소유하므로 nullptr 인 기간(서버 시작 전/종료 후)이 존재할 수 있음.
namespace
{
std::atomic<LibNetworks::Admin::AdminPacketHandler*> g_pAdminHandler { nullptr };

constexpr size_t kSessionBufferSize = 64 * 1024;
constexpr unsigned long kListenBacklog = 1024;
constexpr unsigned int kInitialAcceptCount = 256;
}

// IOCPInboundSession.cpp 에서 extern 으로 선언/호출 (동일 exe 내 링크).
LibNetworks::Admin::AdminPacketHandler* GetGlobalIOCPAdminHandler() noexcept
{
    return g_pAdminHandler.load(std::memory_order_acquire);
}


void IOCPServiceMode::OnStarted()
{
    LibCommons::Logger::GetInstance().LogInfo("IOCPServiceMode", "Starting IOCP Mode...");

    auto pOnFuncCreateSession = [](const std::shared_ptr<LibNetworks::Core::Socket>& pSocket) -> std::shared_ptr<LibNetworks::Sessions::INetworkSession>
        {
            auto pReceiveBuffer = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(kSessionBufferSize);
            auto pSendBuffer = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(kSessionBufferSize);
            return std::make_shared<IOCPInboundSession>(pSocket, std::move(pReceiveBuffer), std::move(pSendBuffer));
        };

    m_Acceptor = LibNetworks::Core::IOSocketAcceptor::Create(
        LibNetworks::Core::Socket::ENetworkMode::IOCP,
        m_ListenSocket,
        pOnFuncCreateSession,
        C_LISTEN_PORT,
        kListenBacklog,
        std::thread::hardware_concurrency() * 2,
        kInitialAcceptCount);
    m_bRunning = nullptr != m_Acceptor;

    using namespace std::chrono_literals;

    // Design Ref: session-idle-timeout §4.4 — SessionIdleChecker.
    LibNetworks::Sessions::IdleCheckerConfig idleCfg;
    idleCfg.thresholdMs    = 60'000ms;
    idleCfg.tickIntervalMs = 1'000ms;
    idleCfg.enabled        = true;

    auto idleSnapshotProvider = []()
        -> std::vector<std::shared_ptr<LibNetworks::Sessions::IIdleAware>>
        {
            std::vector<std::shared_ptr<LibNetworks::Sessions::IIdleAware>> result;
            auto& container = LibCommons::SingleTon<SessionContainer>::GetInstance();
            container.ForEach(
                [&result](uint64_t /*id*/, std::shared_ptr<LibNetworks::Sessions::InboundSession> const& pSession) {
                    if (pSession) {
                        result.push_back(
                            std::static_pointer_cast<LibNetworks::Sessions::IIdleAware>(pSession));
                    }
                });
            return result;
        };

    m_IdleChecker = std::make_shared<LibNetworks::Sessions::SessionIdleChecker>(
        idleCfg, std::move(idleSnapshotProvider));
    m_IdleChecker->Start();

    // Design Ref: server-status §4 — Stats Sampler + Collector + Admin Handler.
    // 순서: Sampler Start → Collector 생성 (Sampler 캐시 사용) → Handler 생성 → 전역 등록.
    LibNetworks::Stats::SamplerConfig samplerCfg;
    samplerCfg.tickIntervalMs = 1'000ms;
    samplerCfg.enabled        = true;
    m_StatsSampler = std::make_shared<LibNetworks::Stats::StatsSampler>(samplerCfg);
    m_StatsSampler->Start();

    auto statsSnapshotProvider = []()
        -> std::vector<std::shared_ptr<LibNetworks::Sessions::ISessionStats>>
        {
            std::vector<std::shared_ptr<LibNetworks::Sessions::ISessionStats>> result;
            auto& container = LibCommons::SingleTon<SessionContainer>::GetInstance();
            container.ForEach(
                [&result](uint64_t /*id*/, std::shared_ptr<LibNetworks::Sessions::InboundSession> const& pSession) {
                    if (pSession) {
                        result.push_back(
                            std::static_pointer_cast<LibNetworks::Sessions::ISessionStats>(pSession));
                    }
                });
            return result;
        };

    auto idleCountProvider = [pChecker = m_IdleChecker]() -> std::uint64_t
        {
            return pChecker ? pChecker->GetDisconnectCount() : 0ULL;
        };

    m_StatsCollector = std::make_shared<LibNetworks::Stats::ServerStatsCollector>(
        LibNetworks::Stats::ServerMode::IOCP,
        std::move(statsSnapshotProvider),
        std::move(idleCountProvider),
        m_StatsSampler.get());

    m_AdminHandler = std::make_shared<LibNetworks::Admin::AdminPacketHandler>(*m_StatsCollector);
    g_pAdminHandler.store(m_AdminHandler.get(), std::memory_order_release);
}


void IOCPServiceMode::OnStopped()
{
    // Design Ref: server-status §6.3 / session-idle-timeout §2.2 — 정리 순서 중요.
    // 전역 핸들러 포인터를 먼저 null 로 set → 신규 admin 패킷 dispatch 차단.
    g_pAdminHandler.store(nullptr, std::memory_order_release);

    if (m_IdleChecker)
    {
        m_IdleChecker->Stop();
        m_IdleChecker.reset();
    }

    if (m_StatsSampler)
    {
        m_StatsSampler->Stop();
    }

    // Handler/Collector/Sampler 순으로 해제 (Handler 는 Collector 참조, Collector 는 Sampler 포인터).
    m_AdminHandler.reset();
    m_StatsCollector.reset();
    m_StatsSampler.reset();

    LibCommons::Logger::GetInstance().LogInfo("IOCPServiceMode", "Stopped.");
}


void IOCPServiceMode::OnShutdown()
{
    g_pAdminHandler.store(nullptr, std::memory_order_release);

    if (m_IdleChecker)
    {
        m_IdleChecker->Stop();
        m_IdleChecker.reset();
    }

    if (m_StatsSampler)
    {
        m_StatsSampler->Stop();
    }

    m_AdminHandler.reset();
    m_StatsCollector.reset();
    m_StatsSampler.reset();

    if (m_Acceptor)
    {
        m_Acceptor->Shutdown();
        m_Acceptor.reset();
    }
}
