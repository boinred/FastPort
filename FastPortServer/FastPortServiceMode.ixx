module;

#include <windows.h>
#include <winnt.h>
#include <spdlog/spdlog.h>

#include <cxxopts.hpp>

export module fastport_service_mode;

import std; 

import commons.service_mode;
import commons.logger; 
import commons.buffers.circle_buffer_queue;
import commons.buffers.external_circle_buffer_queue;

import networks.core.socket; 
import networks.core.io_socket_acceptor;
import networks.services.rio_service;
import networks.core.rio_buffer_manager;
import networks.core.rio_extension;
import networks.sessions.inbound_session;
import networks.sessions.rio_session;
import networks.sessions.inetwork_session;

import fastport_inbound_session;



export class FastPortServiceMode : public LibCommons::ServiceMode
{
public:
    FastPortServiceMode() : ServiceMode(true, true, false) {}

    bool ParseArgs(int argc, const char* argv[])
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
                m_NetworkMode = LibCommons::NetworkMode::RIO;
            }
        }
        catch (const cxxopts::exceptions::exception& e) {
            std::cerr << "Error parsing options: " << e.what() << std::endl;
            return false;
        }
        return true;
    }

protected:
    // ServiceMode을(를) 통해 상속됨
    void OnStarted() override
    {
        LibCommons::Logger::GetInstance().LogInfo("FastPortServiceMode", "OnStarted. Service : {}, Mode : {}", 
            GetDisplayNameAnsi(), (m_NetworkMode == LibCommons::NetworkMode::RIO ? "RIO" : "IOCP"));

        if (m_NetworkMode == LibCommons::NetworkMode::RIO)
        {
            StartRioMode();
        }
        else
        {
            StartIocpMode();
        }
    }

private:
    void StartIocpMode()
    {
        auto pOnFuncCreateSession = [](const std::shared_ptr<LibNetworks::Core::Socket>& pSocket) -> std::shared_ptr<LibNetworks::Sessions::INetworkSession>
            {
                auto pReceiveBuffer = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(8 * 1024);
                auto pSendBuffer = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(8 * 1024);
                auto pSession = std::make_shared<FastPortInboundSession>(pSocket, std::move(pReceiveBuffer), std::move(pSendBuffer));

                return pSession;
            };

        m_Acceptor = LibNetworks::Core::IOSocketAcceptor::Create(m_ListenSocket, pOnFuncCreateSession, C_LISTEN_PORT, 5, std::thread::hardware_concurrency() * 2, 2);
        m_bRunning = nullptr != m_Acceptor;
    }

    void StartRioMode()
    {
        auto& logger = LibCommons::Logger::GetInstance();

        // 1. RIO 확장 로드 (더미 소켓 사용)
        LibNetworks::Core::Socket dummy;
        dummy.CreateSocket();
        if (!LibNetworks::Core::RioExtension::Initialize(dummy.GetSocket()))
        {
            logger.LogError("FastPortServiceMode", "Failed to initialize RIO extension.");
            return;
        }
        dummy.Close();

        // 2. RIO 서비스 및 버퍼 매니저 초기화
        m_RioService = std::make_shared<LibNetworks::Services::RIOService>();
        if (!m_RioService->Initialize(1024)) return;

        m_RioBufferManager = std::make_shared<LibNetworks::Core::RioBufferManager>();
        if (!m_RioBufferManager->Initialize(1024 * 1024 * 64)) return; // 64MB pool

        // 3. 세션 생성 콜백
        auto pOnFuncCreateSession = [this](const std::shared_ptr<LibNetworks::Core::Socket>& pSocket) -> std::shared_ptr<LibNetworks::Sessions::INetworkSession>
            {
                LibNetworks::Core::RioBufferSlice recvSlice, sendSlice;
                m_RioBufferManager->AllocateSlice(8 * 1024, recvSlice);
                m_RioBufferManager->AllocateSlice(8 * 1024, sendSlice);

                auto pSession = std::make_shared<LibNetworks::Sessions::RIOSession>(
                    pSocket, recvSlice, sendSlice, m_RioService->GetCompletionQueue());
                
                pSession->Initialize();
                return pSession;
            };

        // 4. Acceptor 생성 (Accept 과정은 IOCP IOService 사용, 세션은 RIO 사용)
        m_Acceptor = LibNetworks::Core::IOSocketAcceptor::Create(m_ListenSocket, pOnFuncCreateSession, C_LISTEN_PORT, 5, std::thread::hardware_concurrency() * 2, 2);
        
        // RIO 서비스 시작
        m_RioService->Start(std::thread::hardware_concurrency());
        
        m_bRunning = nullptr != m_Acceptor;
    }

protected:
    void OnStopped() override
    {
        LibCommons::Logger::GetInstance().LogInfo("FastPortServiceMode", "OnStopped. Service : {}", GetDisplayNameAnsi());
        if (m_RioService) m_RioService->Stop();
    }

    void OnShutdown() override
    {
        LibCommons::Logger::GetInstance().LogInfo("FastPortServiceMode", "OnShutdown. Service : {}", GetDisplayNameAnsi());

        if (m_Acceptor)
        {
            m_Acceptor->Shutdown();
            m_Acceptor.reset();
        }
    }

    std::wstring GetServiceName() const override { return L"FastPortServer"; }

    std::wstring GetDisplayName() override { return L"FastPortServer Service"; }

    const DWORD GetStartType() const override { return SERVICE_DEMAND_START; }

private:



    const unsigned short C_LISTEN_PORT = 6628;



    LibCommons::NetworkMode m_NetworkMode = LibCommons::NetworkMode::IOCP;



    LibNetworks::Core::Socket m_ListenSocket{};

    std::shared_ptr<LibNetworks::Core::IOSocketAcceptor> m_Acceptor{};



    std::shared_ptr<LibNetworks::Services::RIOService> m_RioService{};

    std::shared_ptr<LibNetworks::Core::RioBufferManager> m_RioBufferManager{};

};
