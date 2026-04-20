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

module rio_service_mode;

import std;
import commons.logger;
import networks.core.rio_extension;
import networks.sessions.inetwork_session;


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
}


void RIOServiceMode::OnStopped()
{
    // 서비스 정지 요청 시: RIO 루프 먼저 종료해서 이후 들어오는 완료 이벤트를 차단.
    // Acceptor 는 OnShutdown 에서 따로 정리.
    if (m_RioService)
    {
        m_RioService->Stop();
        m_RioService.reset();
    }
}


void RIOServiceMode::OnShutdown()
{
    // 시스템 종료 이벤트: Acceptor 를 닫아 새 연결 수락을 멈춘다.
    if (m_Acceptor)
    {
        m_Acceptor->Shutdown();
        m_Acceptor.reset();
    }
}
