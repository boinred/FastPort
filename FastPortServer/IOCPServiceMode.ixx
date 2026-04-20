module;

#include <windows.h>
#include <winnt.h>

export module iocp_service_mode;

import std;
import commons.service_mode;
import networks.core.socket;
import networks.core.io_socket_acceptor;
import networks.sessions.idle_checker;  // SessionIdleChecker
import networks.stats.stats_sampler;
import networks.stats.server_stats_collector;
import networks.admin.admin_packet_handler;
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

    // Design Ref: session-idle-timeout §4.4 — 세션 idle 감지.
    // OnStarted 에서 생성/Start, OnStopped 또는 OnShutdown 에서 Stop.
    std::shared_ptr<LibNetworks::Sessions::SessionIdleChecker> m_IdleChecker{};

    // Design Ref: server-status §4 — Admin 통계/샘플러/핸들러.
    std::shared_ptr<LibNetworks::Stats::StatsSampler>           m_StatsSampler{};
    std::shared_ptr<LibNetworks::Stats::ServerStatsCollector>   m_StatsCollector{};
    std::shared_ptr<LibNetworks::Admin::AdminPacketHandler>     m_AdminHandler{};
};
