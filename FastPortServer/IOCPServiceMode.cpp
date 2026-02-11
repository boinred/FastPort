module;

#include <windows.h>
#include <spdlog/spdlog.h>
#include <thread>

module iocp_service_mode;

import std;
import commons.logger;
import networks.sessions.inetwork_session;

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
}

void IOCPServiceMode::OnStopped()
{
    LibCommons::Logger::GetInstance().LogInfo("IOCPServiceMode", "Stopped.");
}

void IOCPServiceMode::OnShutdown()
{
    if (m_Acceptor)
    {
        m_Acceptor->Shutdown();
        m_Acceptor.reset();
    }
}
