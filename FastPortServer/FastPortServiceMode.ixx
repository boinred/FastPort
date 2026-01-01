module;

#include <windows.h>
#include <winnt.h>
#include <spdlog/spdlog.h>

export module fastport_service_mode;

import std; 

import commons.service_mode;
import commons.logger; 
import commons.buffers.circle_buffer_queue;

import networks.core.socket; 
import networks.core.io_socket_listener;
import networks.sessions.inbound_session;

import fastport_inbound_session;
import commons.container; 

using SessionContainer = LibCommons::Container<uint64_t, std::shared_ptr<LibNetworks::Sessions::InboundSession>>;

export class FastPortServiceMode : public LibCommons::ServiceMode
{
public:
    FastPortServiceMode() : ServiceMode(true, true, false) {}

protected:
    // ServiceMode을(를) 통해 상속됨
    void OnStarted() override
    {
        LibCommons::Logger::GetInstance().LogInfo("FastPortServiceMode", "OnStarted. Service : {}", GetDisplayNameAnsi());

        auto pOnFuncCreateSession = [](const std::shared_ptr<LibNetworks::Core::Socket>& pSocket) -> std::shared_ptr<LibNetworks::Sessions::InboundSession>
            {
                auto pReceiveBuffer = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(8 * 1024);
                auto pSendBuffer = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(8 * 1024);
                auto pSession = std::make_shared<FastPortInboundSession>(pSocket, std::move(pReceiveBuffer), std::move(pSendBuffer));

                auto& sessions = LibCommons::SingleTon<SessionContainer>::GetInstance();
                sessions.Add(pSession->GetSessionId(), pSession);

                return pSession;
            };

        m_pListener = LibNetworks::Core::IOSocketListener::Create(m_ListenSocket, pOnFuncCreateSession, C_LISTEN_PORT, 5, std::thread::hardware_concurrency() * 2, 2);
        m_bRunning = nullptr != m_pListener;
    }

    void OnStopped() override
    {
        LibCommons::Logger::GetInstance().LogInfo("FastPortServiceMode", "OnStopped. Service : {}", GetDisplayNameAnsi());
    }

    void OnShutdown() override
    {
        LibCommons::Logger::GetInstance().LogInfo("FastPortServiceMode", "OnShutdown. Service : {}", GetDisplayNameAnsi());

        if (m_pListener)
        {
            m_pListener->Shutdown();
            m_pListener.reset();
        }
    }

    std::wstring GetServiceName() const override { return L"FastPortServer"; }

    std::wstring GetDisplayName() override { return L"FastPortServer Service"; }

    const DWORD GetStartType() const override { return SERVICE_DEMAND_START; }

private:

    const unsigned short C_LISTEN_PORT = 6628;

    LibNetworks::Core::Socket m_ListenSocket = {};
    std::shared_ptr<LibNetworks::Core::IOSocketListener> m_pListener = {};
};