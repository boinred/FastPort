module;

#include <windows.h>
#include <winnt.h>
#include <stdint.h>
export module rio_service_mode;

import std; 
import commons.service_mode;
import networks.core.socket; 
import networks.core.io_socket_acceptor;
import networks.services.rio_service;
import networks.core.rio_buffer_manager;
import rio_inbound_session;

export class RIOServiceMode : public LibCommons::ServiceMode
{
public:
    RIOServiceMode() : ServiceMode(true, true, false) {}

protected:
    void OnStarted() override;
    void OnStopped() override;
    void OnShutdown() override;

    std::wstring GetServiceName() const override { return L"FastPortServerRIO"; }
    std::wstring GetDisplayName() override { return L"FastPortServer RIO Service"; }
    const DWORD GetStartType() const override { return SERVICE_DEMAND_START; }

private:
    const unsigned short C_LISTEN_PORT = 6628;
    const uint32_t C_RIO_RECV_BUFFER_SIZE = 16 * 1024;
    const uint32_t C_RIO_SEND_BUFFER_SIZE = 16 * 1024;

    LibNetworks::Core::Socket m_ListenSocket{};
    std::shared_ptr<LibNetworks::Core::IOSocketAcceptor> m_Acceptor{};
    std::shared_ptr<LibNetworks::Services::RIOService> m_RioService{};
    std::shared_ptr<LibNetworks::Core::RioBufferManager> m_RioBufferManager{};
};