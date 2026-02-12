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

    // RIO 확장 로드
    LibNetworks::Core::Socket dummy;
    dummy.CreateSocket(LibNetworks::Core::Socket::ENetworkMode::RIO);
    if (!LibNetworks::Core::RioExtension::Initialize(dummy.GetSocket()))
    {
        logger.LogError("RIOServiceMode", "Failed to initialize RIO extension.");
        return;
    }
    dummy.Close();

    // RIO 서비스 초기화
    m_RioService = std::make_shared<LibNetworks::Services::RIOService>();
    if (!m_RioService->Initialize(1024))
    {
        logger.LogError("RIOServiceMode", "Failed to initialize RIO service.");
        return;
    }

    m_RioBufferManager = std::make_shared<LibNetworks::Core::RioBufferManager>();
    if (!m_RioBufferManager->Initialize(1024 * 1024 * 64))
    {
        logger.LogError("RIOServiceMode", "Failed to initialize RIO buffer manager.");
        return;
    }

    auto pOnFuncCreateSession = [this](const std::shared_ptr<LibNetworks::Core::Socket>& pSocket) -> std::shared_ptr<LibNetworks::Sessions::INetworkSession>
        {
            LibNetworks::Core::RioBufferSlice recvSlice, sendSlice;
            m_RioBufferManager->AllocateSlice(C_RIO_RECV_BUFFER_SIZE, recvSlice);
            m_RioBufferManager->AllocateSlice(C_RIO_SEND_BUFFER_SIZE, sendSlice);

            auto pSession = std::make_shared<RIOInboundSession>(pSocket, recvSlice, sendSlice, m_RioService->GetCompletionQueue());
            pSession->Initialize();
            return pSession;
        };

    m_Acceptor = LibNetworks::Core::IOSocketAcceptor::Create(LibNetworks::Core::Socket::ENetworkMode::RIO, m_ListenSocket, pOnFuncCreateSession, C_LISTEN_PORT, 5, std::thread::hardware_concurrency() * 2, 2);

    //m_RioService->Start(std::thread::hardware_concurrency());
    m_RioService->Start(1);

    m_bRunning = nullptr != m_Acceptor;
}

void RIOServiceMode::OnStopped()
{
    if (m_RioService)
    {
        m_RioService->Stop();
        m_RioService.reset();
    }
}

void RIOServiceMode::OnShutdown()
{
    if (m_Acceptor)
    {
        m_Acceptor->Shutdown();
        m_Acceptor.reset();
    }
}
