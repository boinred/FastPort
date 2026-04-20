// RIOServiceMode.cpp
// -----------------------------------------------------------------------------
// RIOServiceMode 의 구현. Windows Service 생명주기에 맞춰 RIO 스택을 구성/해체한다.
//
// 시작 순서(OnStarted):
//   1) 더미 소켓으로 RIO 확장 함수 포인터 테이블 로드 (프로세스당 1회 필요)
//   2) RIOService 초기화 — RIO CompletionQueue 생성, 결과 루프 스레드 준비
//   3) RioBufferManager 초기화 — 64MB 전역 고정 버퍼 풀 pre-allocate
//   4) Acceptor 생성 + createSession 콜백 등록
//   5) RIOService 루프 시작 (워커 스레드 기동)
//
// 종료 순서: Stopped → RIOService.Stop() 먼저, Shutdown → Acceptor 차단.
// -----------------------------------------------------------------------------
module;

#include <windows.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>

module rio_service_mode;

import std;
import commons.logger;
import commons.container;
import commons.singleton;
import networks.core.rio_extension;
import networks.sessions.inetwork_session;
import networks.sessions.rio_session;
import networks.sessions.isession_stats;


// RIO 세션 컨테이너. RIOInboundSession.cpp 와 동일 타입이어야 SingleTon 공유.
using RIOSessionContainer = LibCommons::Container<
    uint64_t,
    std::shared_ptr<LibNetworks::Sessions::RIOSession>>;


// 전역 AdminPacketHandler 액세스 포인트 (RIO 서버용).
namespace
{
std::atomic<LibNetworks::Admin::AdminPacketHandler*> g_pRIOAdminHandler { nullptr };
}

LibNetworks::Admin::AdminPacketHandler* GetGlobalRIOAdminHandler() noexcept
{
    return g_pRIOAdminHandler.load(std::memory_order_acquire);
}


void RIOServiceMode::OnStarted()
{
    auto& logger = LibCommons::Logger::GetInstance();
    logger.LogInfo("RIOServiceMode", "Starting RIO Mode...");

    // --- Step 1: RIO 확장 함수 포인터 테이블 로드 ---
    // WSAIoctl(SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER) 는 유효한 소켓이 필요.
    // 실제로는 사용하지 않는 임시 소켓을 만들어 RIO 함수 테이블만 뽑은 뒤 닫는다.
    LibNetworks::Core::Socket dummy;
    dummy.CreateSocket(LibNetworks::Core::Socket::ENetworkMode::RIO);
    if (!LibNetworks::Core::RioExtension::Initialize(dummy.GetSocket()))
    {
        logger.LogError("RIOServiceMode", "Failed to initialize RIO extension.");
        return;
    }
    dummy.Close();

    // --- Step 2: RIO 서비스(CompletionQueue + 루프) 초기화 ---
    // 인자 1024 = CQ 엔트리 수. 세션 수와 요청 수를 고려해 설정.
    m_RioService = std::make_shared<LibNetworks::Services::RIOService>();
    if (!m_RioService->Initialize(1024))
    {
        logger.LogError("RIOServiceMode", "Failed to initialize RIO service.");
        return;
    }

    // --- Step 3: RIO 고정 버퍼 풀 초기화 (64MB) ---
    // 세션 수×(recv+send)×여유율을 고려. 16KB×2×약 2000 세션 기준.
    m_RioBufferManager = std::make_shared<LibNetworks::Core::RioBufferManager>();
    if (!m_RioBufferManager->Initialize(1024 * 1024 * 64))
    {
        logger.LogError("RIOServiceMode", "Failed to initialize RIO buffer manager.");
        return;
    }

    // --- Step 4: 새 연결 발생 시 세션 생성 콜백 정의 ---
    // Acceptor 가 AcceptEx 완료 시 이 람다를 호출해 RIOInboundSession 을 만든다.
    auto pOnFuncCreateSession = [this](const std::shared_ptr<LibNetworks::Core::Socket>& pSocket) -> std::shared_ptr<LibNetworks::Sessions::INetworkSession>
        {
            // 세션당 수신/송신 버퍼 슬라이스를 전역 풀에서 할당 받는다.
            // RIO 는 커널에 등록된 버퍼만 허용하므로 slice 가 필수.
            LibNetworks::Core::RioBufferSlice recvSlice, sendSlice;
            m_RioBufferManager->AllocateSlice(C_RIO_RECV_BUFFER_SIZE, recvSlice);
            m_RioBufferManager->AllocateSlice(C_RIO_SEND_BUFFER_SIZE, sendSlice);

            auto pSession = std::make_shared<RIOInboundSession>(pSocket, recvSlice, sendSlice, m_RioService->GetCompletionQueue());
            pSession->Initialize();
            return pSession;
        };

    // Acceptor 파라미터: (mode, listenSocket, createSessionCb, port, backlog=5,
    //                    acceptThreads=hw*2, pendingAccepts=2)
    m_Acceptor = LibNetworks::Core::IOSocketAcceptor::Create(LibNetworks::Core::Socket::ENetworkMode::RIO, m_ListenSocket, pOnFuncCreateSession, C_LISTEN_PORT, 5, std::thread::hardware_concurrency() * 2, 2);

    // --- Step 5: RIO 서비스 루프 시작 ---
    // 주의: 스레드 수는 튜닝 포인트. 현재는 단일 워커.
    //       hardware_concurrency 로 올릴 경우 RIO CQ 의 순서/공정성 영향 검토 필요.
    //m_RioService->Start(std::thread::hardware_concurrency());
    m_RioService->Start(1);

    m_bRunning = nullptr != m_Acceptor;

    // Design Ref: server-status §4 — Stats Sampler + Collector + Admin Handler 연동 (RIO).
    using namespace std::chrono_literals;
    LibNetworks::Stats::SamplerConfig samplerCfg;
    samplerCfg.tickIntervalMs = 1'000ms;
    samplerCfg.enabled        = true;
    m_StatsSampler = std::make_shared<LibNetworks::Stats::StatsSampler>(samplerCfg);
    m_StatsSampler->Start();

    auto statsSnapshotProvider = []()
        -> std::vector<std::shared_ptr<LibNetworks::Sessions::ISessionStats>>
        {
            std::vector<std::shared_ptr<LibNetworks::Sessions::ISessionStats>> result;
            auto& container = LibCommons::SingleTon<RIOSessionContainer>::GetInstance();
            container.ForEach(
                [&result](uint64_t /*id*/, std::shared_ptr<LibNetworks::Sessions::RIOSession> const& pSession) {
                    if (pSession) {
                        result.push_back(
                            std::static_pointer_cast<LibNetworks::Sessions::ISessionStats>(pSession));
                    }
                });
            return result;
        };

    // RIO 는 session-idle-timeout 미적용 → IdleCountProvider 는 0 상수.
    auto idleCountProvider = []() -> std::uint64_t { return 0ULL; };

    m_StatsCollector = std::make_shared<LibNetworks::Stats::ServerStatsCollector>(
        LibNetworks::Stats::ServerMode::RIO,
        std::move(statsSnapshotProvider),
        std::move(idleCountProvider),
        m_StatsSampler.get());

    m_AdminHandler = std::make_shared<LibNetworks::Admin::AdminPacketHandler>(*m_StatsCollector);
    g_pRIOAdminHandler.store(m_AdminHandler.get(), std::memory_order_release);
}


void RIOServiceMode::OnStopped()
{
    // Admin 전역 포인터 먼저 무효화.
    g_pRIOAdminHandler.store(nullptr, std::memory_order_release);

    if (m_StatsSampler)
    {
        m_StatsSampler->Stop();
    }
    m_AdminHandler.reset();
    m_StatsCollector.reset();
    m_StatsSampler.reset();

    // RIO 루프 먼저 종료.
    if (m_RioService)
    {
        m_RioService->Stop();
        m_RioService.reset();
    }
}


void RIOServiceMode::OnShutdown()
{
    g_pRIOAdminHandler.store(nullptr, std::memory_order_release);

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
