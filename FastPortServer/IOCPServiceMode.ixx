module;

#include <windows.h>
#include <winnt.h>

export module iocp_service_mode;

import std; 
import commons.service_mode;
import networks.core.socket; 
import networks.core.io_socket_acceptor;
import iocp_inbound_session;
import commons.buffers.circle_buffer_queue;

export class IOCPServiceMode : public LibCommons::ServiceMode
{
public:
    IOCPServiceMode() : ServiceMode(true, true, false) {}

protected:
    void OnStarted() override;
    void OnStopped() override;
    void OnShutdown() override;

    std::wstring GetServiceName() const override { return L"FastPortServerIOCP"; }
    std::wstring GetDisplayName() override { return L"FastPortServer IOCP Service"; }
    const DWORD GetStartType() const override { return SERVICE_DEMAND_START; }

private:
    const unsigned short C_LISTEN_PORT = 6628;
    LibNetworks::Core::Socket m_ListenSocket{};
    std::shared_ptr<LibNetworks::Core::IOSocketAcceptor> m_Acceptor{};
};