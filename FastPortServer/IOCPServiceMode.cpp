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


// Design Ref: session-idle-timeout §4.4 — 서버에서 활성 세션을 보관하는 전역 컨테이너.
// IOCPInboundSession.cpp 의 정의와 동일 타입이어야 동일 싱글톤을 공유.
// NOTE: `using` alias 는 translation-unit scope 이므로 별칭 이름은 달라도
// `SingleTon<LibCommons::Container<uint64_t, std::shared_ptr<InboundSession>>>::GetInstance()` 는
// 전역 동일 인스턴스. IOCPInboundSession 이 Add/Remove, 여기는 ForEach 순회.
using SessionContainer = LibCommons::Container<
    uint64_t,
    std::shared_ptr<LibNetworks::Sessions::InboundSession>>;


void IOCPServiceMode::OnStarted()
{
    LibCommons::Logger::GetInstance().LogInfo("IOCPServiceMode", "Starting IOCP Mode...");

    auto pOnFuncCreateSession = [](const std::shared_ptr<LibNetworks::Core::Socket>& pSocket) -> std::shared_ptr<LibNetworks::Sessions::INetworkSession>
        {
            auto pReceiveBuffer = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(8 * 1024);
            auto pSendBuffer = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(8 * 1024);
            return std::make_shared<IOCPInboundSession>(pSocket, std::move(pReceiveBuffer), std::move(pSendBuffer));
        };

    m_Acceptor = LibNetworks::Core::IOSocketAcceptor::Create(LibNetworks::Core::Socket::ENetworkMode::IOCP, m_ListenSocket, pOnFuncCreateSession, C_LISTEN_PORT, 5, std::thread::hardware_concurrency() * 2, 2);
    m_bRunning = nullptr != m_Acceptor;

    // Design Ref: session-idle-timeout §4.4 — SessionIdleChecker 설치.
    // 기본값: 10초 idle, 1초 tick, enabled. 운영 중 조정 필요 시 여기서 Config 변경.
    using namespace std::chrono_literals;
    LibNetworks::Sessions::IdleCheckerConfig cfg;
    cfg.thresholdMs    = 10'000ms;
    cfg.tickIntervalMs = 1'000ms;
    cfg.enabled        = true;

    // SnapshotProvider: SessionContainer 에서 활성 세션을 IIdleAware 로 upcast 해 수집.
    // InboundSession → IOSession → IIdleAware 상속 체인이 성립하므로 static_pointer_cast 안전.
    auto snapshotProvider = []()
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
        cfg, std::move(snapshotProvider));
    m_IdleChecker->Start();
}


void IOCPServiceMode::OnStopped()
{
    // Design Ref: session-idle-timeout §2.2 Shutdown sequence — Checker 정지를 먼저 수행.
    // TimerQueue::Cancel(wait=true) 로 진행 중 tick 완료 대기. 이후 Acceptor 정리는 OnShutdown 에서.
    if (m_IdleChecker)
    {
        m_IdleChecker->Stop();
        m_IdleChecker.reset();
    }

    LibCommons::Logger::GetInstance().LogInfo("IOCPServiceMode", "Stopped.");
}


void IOCPServiceMode::OnShutdown()
{
    // 방어적: OnStopped 가 호출되지 않고 바로 Shutdown 이 오는 경로 대비.
    if (m_IdleChecker)
    {
        m_IdleChecker->Stop();
        m_IdleChecker.reset();
    }

    if (m_Acceptor)
    {
        m_Acceptor->Shutdown();
        m_Acceptor.reset();
    }
}
