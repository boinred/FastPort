// Design Ref: §3.6 — ABCompare (A/B comparison mode: IOCP vs RIO simultaneous test)
// Plan SC: SC4 (A/B 비교 모드에서 IOCP vs RIO 나란히 차트 렌더링)
module;

#include <WinSock2.h>
#include <MSWSock.h>
#include <memory>
#include <string>
#include <functional>
#include <spdlog/spdlog.h>

export module test_client.ab_compare;

import test_client.test_runner;
import test_client.metrics_collector;

export class ABCompare
{
public:
    using LogCallback = std::function<void(const char*)>;

    ABCompare() = default;
    ~ABCompare() { Stop(); }

    void SetLogCallback(LogCallback cb)
    {
        m_LogCallback = std::move(cb);
        m_RunnerA.SetLogCallback(m_LogCallback);
        m_RunnerB.SetLogCallback(m_LogCallback);
    }

    // 2개 서버에 동시 연결 (A=IOCP 서버, B=RIO 서버)
    bool Start(const char* ipA, int portA, const char* ipB, int portB)
    {
        m_MetricsA.Reset();
        m_MetricsB.Reset();
        m_RunnerA.SetMetrics(&m_MetricsA);
        m_RunnerB.SetMetrics(&m_MetricsB);

        bool a = m_RunnerA.Connect(ipA, portA);
        bool b = m_RunnerB.Connect(ipB, portB);

        m_bRunning = a && b;

        if (!m_bRunning)
        {
            m_RunnerA.Disconnect();
            m_RunnerB.Disconnect();
        }

        if (!a && m_LogCallback) m_LogCallback("[A/B] Server A connection failed");
        if (!b && m_LogCallback) m_LogCallback("[A/B] Server B connection failed");
        if (m_bRunning && m_LogCallback) m_LogCallback("[A/B] Both servers connected");

        return m_bRunning;
    }

    // 동일 워크로드 동시 수행
    void RunParallelEcho(int count)
    {
        if (!m_bRunning) return;
        m_RunnerA.RunEchoTest(count);
        m_RunnerB.RunEchoTest(count);
    }

    void RunParallelScale(int connections)
    {
        if (!m_bRunning) return;
        // Scale은 Start 시 사용한 IP/Port가 필요하므로 저장
        m_RunnerA.ConnectScale(m_IpA.c_str(), m_PortA, connections);
        m_RunnerB.ConnectScale(m_IpB.c_str(), m_PortB, connections);
    }

    void RunParallelFlood(int durationSec)
    {
        if (!m_bRunning) return;
        m_RunnerA.RunFloodTest(durationSec);
        m_RunnerB.RunFloodTest(durationSec);
    }

    void Stop()
    {
        m_RunnerA.Disconnect();
        m_RunnerB.Disconnect();
        m_bRunning = false;
    }

    bool IsRunning() const { return m_bRunning; }

    MetricsCollector& GetMetricsA() { return m_MetricsA; }
    MetricsCollector& GetMetricsB() { return m_MetricsB; }
    const MetricsCollector& GetMetricsA() const { return m_MetricsA; }
    const MetricsCollector& GetMetricsB() const { return m_MetricsB; }

    // Start 시 주소 저장 (Scale 테스트용)
    bool StartWithAddresses(const char* ipA, int portA, const char* ipB, int portB)
    {
        m_IpA = ipA; m_PortA = portA;
        m_IpB = ipB; m_PortB = portB;
        return Start(ipA, portA, ipB, portB);
    }

private:
    MetricsCollector m_MetricsA;
    MetricsCollector m_MetricsB;
    TestRunner m_RunnerA;
    TestRunner m_RunnerB;
    LogCallback m_LogCallback;
    bool m_bRunning = false;

    std::string m_IpA;
    int m_PortA = 0;
    std::string m_IpB;
    int m_PortB = 0;
};
