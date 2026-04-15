// Design Ref: §3.5 — TestRunner (connection + echo/flood/scale tests)
// Plan SC: SC1 (연결), SC3 (스케일 테스트)
module;

#include <WinSock2.h>
#include <MSWSock.h>
#include <memory>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <functional>
#include <spdlog/spdlog.h>

export module test_client.test_runner;

import networks.services.io_service;
import networks.core.io_socket_connector;
import networks.core.socket;
import networks.sessions.outbound_session;
import commons.buffers.circle_buffer_queue;
import commons.event_listener;
import test_client.metrics_collector;
import test_client.test_session;

export class TestRunner
{
public:
    using LogCallback = std::function<void(const char*)>;

    TestRunner() = default;
    ~TestRunner() { Disconnect(); }

    void SetMetrics(MetricsCollector* pMetrics) { m_pMetrics = pMetrics; }
    void SetLogCallback(LogCallback cb) { m_LogCallback = std::move(cb); }

    bool Connect(const char* ip, int port)
    {
        if (m_bConnected) return true;

        m_pIOService = std::make_shared<LibNetworks::Services::IOService>();
        m_pIOService->Start(std::thread::hardware_concurrency());

        auto pMetrics = m_pMetrics;
        auto logCb = m_LogCallback;

        auto pOnCreateSession = [pMetrics, logCb](const std::shared_ptr<LibNetworks::Core::Socket>& pSocket)
            -> std::shared_ptr<LibNetworks::Sessions::OutboundSession>
            {
                const size_t bufferSize = 8 * 1024;
                auto pRecv = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(bufferSize);
                auto pSend = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(bufferSize);
                auto pSession = std::make_shared<TestSession>(pSocket, std::move(pRecv), std::move(pSend));
                pSession->SetMetrics(pMetrics);
                pSession->SetLogCallback(logCb);
                return pSession;
            };

        auto pConnector = LibNetworks::Core::IOSocketConnector::Create(
            m_pIOService, pOnCreateSession, std::string(ip), static_cast<unsigned short>(port));

        if (!pConnector)
        {
            return false;
        }

        m_Connectors.push_back(pConnector);
        m_bConnected = true;
        return true;
    }

    void Disconnect()
    {
        for (auto& c : m_Connectors)
        {
            if (c) c->DisConnect();
        }
        m_Connectors.clear();

        if (m_pIOService)
        {
            m_pIOService->Stop();
            m_pIOService.reset();
        }
        m_bConnected = false;
    }

    bool IsConnected() const { return m_bConnected; }

    bool ConnectScale(const char* ip, int port, int count)
    {
        if (!m_pIOService)
        {
            m_pIOService = std::make_shared<LibNetworks::Services::IOService>();
            m_pIOService->Start(std::thread::hardware_concurrency());
        }

        auto pMetrics = m_pMetrics;
        auto logCb = m_LogCallback;

        auto pOnCreateSession = [pMetrics, logCb](const std::shared_ptr<LibNetworks::Core::Socket>& pSocket)
            -> std::shared_ptr<LibNetworks::Sessions::OutboundSession>
            {
                const size_t bufferSize = 8 * 1024;
                auto pRecv = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(bufferSize);
                auto pSend = std::make_unique<LibCommons::Buffers::CircleBufferQueue>(bufferSize);
                auto pSession = std::make_shared<TestSession>(pSocket, std::move(pRecv), std::move(pSend));
                pSession->SetMetrics(pMetrics);
                pSession->SetLogCallback(logCb);
                return pSession;
            };

        int success = 0;
        for (int i = 0; i < count; ++i)
        {
            auto pConnector = LibNetworks::Core::IOSocketConnector::Create(
                m_pIOService, pOnCreateSession, std::string(ip), static_cast<unsigned short>(port));
            if (pConnector)
            {
                m_Connectors.push_back(pConnector);
                ++success;
            }
        }

        m_bConnected = (success > 0);
        return m_bConnected;
    }

    int GetConnectionCount() const { return static_cast<int>(m_Connectors.size()); }

private:
    MetricsCollector* m_pMetrics = nullptr;
    LogCallback m_LogCallback;
    std::shared_ptr<LibNetworks::Services::IOService> m_pIOService;
    std::vector<std::shared_ptr<LibNetworks::Core::IOSocketConnector>> m_Connectors;
    bool m_bConnected = false;
};
