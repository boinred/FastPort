module;

#include <windows.h>
#include <winnt.h>
#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

module fastport_service_mode;

import std;
import commons.logger;
import commons.buffers.circle_buffer_queue;
import networks.core.socket;
import networks.core.io_socket_acceptor;
import networks.services.rio_service;
import networks.core.rio_buffer_manager;
import networks.core.rio_extension;
import networks.sessions.rio_session;
import networks.sessions.inetwork_session;
import fastport_inbound_session;

bool FastPortServiceMode::ParseArgs(int argc, const char* argv[])
{
    try {
        cxxopts::Options options("FastPortServer", "High-performance network server");
        options.add_options()
            ("r,rio", "Use RIO (Registered I/O) mode", cxxopts::value<bool>()->default_value("false"))
            ("h,help", "Print usage");

        auto result = options.parse(argc, argv);

        if (result.count("help")) {
            std::cout << options.help() << std::endl;
            return false;
        }

        if (result["rio"].as<bool>()) {
            m_NetworkMode = LibNetworks::Core::Socket::ENetworkMode::RIO;
        }
    }
    catch (const cxxopts::exceptions::exception& e) {
        std::cerr << "Error parsing options: " << e.what() << std::endl;
        return false;
    }
    return true;
}

void FastPortServiceMode::OnStarted()
{
    LibCommons::Logger::GetInstance().LogInfo("FastPortServiceMode", "OnStarted. Service : {}, Mode : {}",
        GetDisplayNameAnsi(), (m_NetworkMode == LibNetworks::Core::Socket::ENetworkMode::RIO ? "RIO" : "IOCP"));

    if (m_NetworkMode == LibNetworks::Core::Socket::ENetworkMode::RIO)
    {
        StartRioMode();
    }
    else
    {
        StartIocpMode();
    }
}

void FastPortServiceMode::StartIocpMode()
{

    auto pOnFuncCreateSession = [](const std::shared_ptr<LibNetworks::Core::Socket>& pSocket) -> std::shared_ptr<LibNetworks::Sessions::INetworkSession>
        {
            auto pReceiveBuffer = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(8 * 1024);  // 8KB
            auto pSendBuffer = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(8 * 1024);  // 8KB  
            auto pSession = std::make_shared<FastPortInboundSession>(pSocket, std::move(pReceiveBuffer), std::move(pSendBuffer));

            return pSession;
        };

    m_Acceptor = LibNetworks::Core::IOSocketAcceptor::Create(m_ListenSocket, pOnFuncCreateSession, C_LISTEN_PORT, 5, std::thread::hardware_concurrency() * 2, 2);
    m_bRunning = nullptr != m_Acceptor;
}

void FastPortServiceMode::StartRioMode()
{
    auto& logger = LibCommons::Logger::GetInstance();

    // 1. RIO 확장 로드 (더미 소켓 사용)
    LibNetworks::Core::Socket dummy;
    dummy.CreateSocket(LibNetworks::Core::Socket::ENetworkMode::RIO);

    if (!LibNetworks::Core::RioExtension::Initialize(dummy.GetSocket()))
    {
        logger.LogError("FastPortServiceMode", "Failed to initialize RIO extension.");
        return;
    }
    dummy.Close();

    // 2. RIO 서비스 및 버퍼 매니저 초기화
    m_RioService = std::make_shared<LibNetworks::Services::RIOService>();
    if (!m_RioService->Initialize(1024))
    {
        logger.LogError("FastPortServiceMode", "Failed to initialize RIO service.");
        return;
    }

    m_RioBufferManager = std::make_shared<LibNetworks::Core::RioBufferManager>();

    // TIPS: 실제 서버에서는 여유분을 고려해 보통 512MB ~ 1GB 정도를 할당하여 운영합니다.
    if (!m_RioBufferManager->Initialize(1024 * 1024 * 64)) // 64MB
    {
        // 세션 1개당 16KB (수신 8KB + 송신 8KB) 사용 시 약 4096개 동시 세션 지원
        logger.LogError("FastPortServiceMode", "Failed to initialize RIO buffer manager.");
        return; // 64MB pool
    }

    // 3. 세션 생성 콜백
    auto pOnFuncCreateSession = [this](const std::shared_ptr<LibNetworks::Core::Socket>& pSocket) -> std::shared_ptr<LibNetworks::Sessions::INetworkSession>
        {
            LibNetworks::Core::RioBufferSlice recvSlice, sendSlice;
            m_RioBufferManager->AllocateSlice(C_RIO_RECV_BUFFER_SIZE, recvSlice);
            m_RioBufferManager->AllocateSlice(C_RIO_SEND_BUFFER_SIZE, sendSlice);

            auto pSession = std::make_shared<LibNetworks::Sessions::RIOSession>(pSocket, recvSlice, sendSlice, m_RioService->GetCompletionQueue());

            pSession->Initialize();
            return pSession;
        };

    // 4. Acceptor 생성 (Accept 과정은 IOCP IOService 사용, 세션은 RIO 사용)
    m_Acceptor = LibNetworks::Core::IOSocketAcceptor::Create(m_ListenSocket, pOnFuncCreateSession, C_LISTEN_PORT, 5, std::thread::hardware_concurrency() * 2, 2);

    // RIO 서비스 시작
    m_RioService->Start(std::thread::hardware_concurrency());

    m_bRunning = nullptr != m_Acceptor;
}

void FastPortServiceMode::OnStopped()
{
    LibCommons::Logger::GetInstance().LogInfo("FastPortServiceMode", "OnStopped. Service : {}", GetDisplayNameAnsi());
    if (m_RioService)
    {
        m_RioService->Stop();
        m_RioService.reset();
    }
}

void FastPortServiceMode::OnShutdown()
{
    LibCommons::Logger::GetInstance().LogInfo("FastPortServiceMode", "OnShutdown. Service : {}", GetDisplayNameAnsi());

    if (m_Acceptor)
    {
        m_Acceptor->Shutdown();
        m_Acceptor.reset();
    }
}
