// Design Ref: §3.2 — TestClientApp (UI layout + app state management)
// Plan SC: SC1-SC6 (전체 UI 통합)
module;

#include <imgui.h>
#include <implot.h>
#include <cstdio>
#include <vector>
#include <string>
#include <deque>
#include <mutex>
#include <chrono>
#include <functional>
#include "Protocols/Admin.pb.h"

export module test_client.app;

import test_client.metrics_collector;
import test_client.test_runner;
import test_client.ab_compare;

export class TestClientApp
{
public:
    using LogCallback = std::function<void(const char*)>;

    // Design Ref: §3.2 — AppState enum for explicit state management
    enum class AppState { Disconnected, Connecting, Connected, Testing };

    void Initialize(LogCallback logCb)
    {
        m_LogCallback = std::move(logCb);
        m_Runner.SetMetrics(&m_Metrics);
        m_Runner.SetLogCallback(m_LogCallback);
        m_ABCompare.SetLogCallback(m_LogCallback);

        // Design Ref: server-status §5.2 — Admin 응답 수신 콜백 등록.
        m_Runner.SetAdminSummaryCallback(
            [this](const ::fastport::protocols::admin::AdminStatusSummaryResponse& res) {
                std::lock_guard lock(m_AdminMutex);
                m_AdminSummary = res;
                m_bAdminSummaryValid = true;
            });
        m_Runner.SetAdminSessionListCallback(
            [this](const ::fastport::protocols::admin::AdminSessionListResponse& res) {
                std::lock_guard lock(m_AdminMutex);
                m_AdminSessionList = res;
                m_bAdminSessionListValid = true;
            });
    }

    void Render()
    {
        // 상태 자동 갱신
        UpdateState();

        auto snap = m_Metrics.GetSnapshot();

        // Left panel: Controls
        ImGui::BeginChild("Controls", ImVec2(260, 0), true);
        RenderControlsPanel();
        ImGui::EndChild();

        ImGui::SameLine();

        // Right panel: Tabs (Metrics / Admin)
        ImGui::BeginChild("Main", ImVec2(0, 0), false);
        {
            if (m_bABMode && m_ABCompare.IsRunning())
            {
                RenderABComparePanel();
            }
            else if (ImGui::BeginTabBar("MainTabs"))
            {
                if (ImGui::BeginTabItem("Metrics"))
                {
                    RenderMetricsPanel(snap);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Admin"))
                {
                    RenderAdminPanel();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Stress"))
                {
                    RenderStressPanel();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::EndChild();
    }

    void Shutdown()
    {
        m_Runner.Disconnect();
        m_ABCompare.Stop();
        m_State = AppState::Disconnected;
    }

    AppState GetState() const { return m_State; }
    MetricsCollector& GetMetrics() { return m_Metrics; }

private:
    void UpdateState()
    {
        if (!m_Runner.IsConnected())
            m_State = AppState::Disconnected;
        else if (m_Runner.IsTestRunning())
            m_State = AppState::Testing;
        else
            m_State = AppState::Connected;
    }

    void RenderControlsPanel()
    {
        // -- Connection --
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "CONNECTION");
        ImGui::Separator();
        ImGui::InputText("Server", m_ServerIP, sizeof(m_ServerIP));
        ImGui::InputInt("Port", &m_ServerPort);
        ImGui::RadioButton("IOCP", &m_EngineMode, 0); ImGui::SameLine();
        ImGui::RadioButton("RIO", &m_EngineMode, 1);

        if (!m_Runner.IsConnected())
        {
            if (ImGui::Button("Connect", ImVec2(-1, 0)))
            {
                Log(m_EngineMode == 0 ? "Connecting (IOCP)..." : "Connecting (RIO)...");
                if (m_Runner.Connect(m_ServerIP, m_ServerPort))
                    Log("Connection initiated");
                else
                    Log("Connection failed!");
            }
        }
        else
        {
            if (ImGui::Button("Disconnect", ImVec2(-1, 0)))
            {
                m_Runner.Disconnect();
                m_Metrics.Reset();
                Log("Disconnected");
            }
        }

        // -- Tests --
        ImGui::Spacing(); ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "TESTS");
        ImGui::Separator();

        ImGui::InputInt("Echo Count", &m_EchoCount);
        if (ImGui::Button("Run Echo Test", ImVec2(-1, 0)))
        {
            char buf[64];
            snprintf(buf, sizeof(buf), "Echo test: %d messages", m_EchoCount);
            Log(buf);
            m_Runner.RunEchoTest(m_EchoCount);
        }

        ImGui::Spacing();
        ImGui::Text("Scale Test:");
        if (ImGui::Button("1", ImVec2(50, 0)))    { m_Runner.ConnectScale(m_ServerIP, m_ServerPort, 1); Log("Scale: +1"); }
        ImGui::SameLine();
        if (ImGui::Button("10", ImVec2(50, 0)))   { m_Runner.ConnectScale(m_ServerIP, m_ServerPort, 10); Log("Scale: +10"); }
        ImGui::SameLine();
        if (ImGui::Button("100", ImVec2(50, 0)))  { m_Runner.ConnectScale(m_ServerIP, m_ServerPort, 100); Log("Scale: +100"); }
        ImGui::SameLine();
        if (ImGui::Button("1000", ImVec2(50, 0))) { m_Runner.ConnectScale(m_ServerIP, m_ServerPort, 1000); Log("Scale: +1000"); }

        ImGui::Spacing();
        // Flood test
        if (!m_Runner.IsFloodRunning())
        {
            if (ImGui::Button("Flood 10s", ImVec2(-1, 0)))
            {
                Log("Flood test: 10 seconds");
                m_Runner.RunFloodTest(10);
            }
        }
        else
        {
            if (ImGui::Button("Stop Flood", ImVec2(-1, 0)))
            {
                m_Runner.StopFlood();
                Log("Flood test stopped");
            }
        }

        // -- A/B Compare --
        ImGui::Spacing(); ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "A/B COMPARE");
        ImGui::Separator();
        ImGui::InputText("IP A", m_ABIpA, sizeof(m_ABIpA));
        ImGui::InputInt("Port A (IOCP)", &m_ABPortA);
        ImGui::InputText("IP B", m_ABIpB, sizeof(m_ABIpB));
        ImGui::InputInt("Port B (RIO)", &m_ABPortB);

        if (!m_ABCompare.IsRunning())
        {
            if (ImGui::Button("Start A/B", ImVec2(-1, 0)))
            {
                Log("[A/B] Starting comparison...");
                if (m_ABCompare.StartWithAddresses(m_ABIpA, m_ABPortA, m_ABIpB, m_ABPortB))
                {
                    m_bABMode = true;
                    Log("[A/B] Connected to both servers");
                }
                else
                    Log("[A/B] Failed to connect");
            }
        }
        else
        {
            if (ImGui::Button("Stop A/B", ImVec2(-1, 0)))
            {
                m_ABCompare.Stop();
                m_bABMode = false;
                Log("[A/B] Stopped");
            }
            ImGui::InputInt("A/B Echo", &m_ABEchoCount);
            if (ImGui::Button("A/B Echo Test", ImVec2(-1, 0)))
            {
                m_ABCompare.RunParallelEcho(m_ABEchoCount);
                Log("[A/B] Parallel echo started");
            }
            if (ImGui::Button("A/B Flood 10s", ImVec2(-1, 0)))
            {
                m_ABCompare.RunParallelFlood(10);
                Log("[A/B] Parallel flood started");
            }
        }

        // -- Status --
        ImGui::Spacing(); ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "STATUS");
        ImGui::Separator();
        const char* stateStr = "Unknown";
        ImVec4 stateColor = ImVec4(1, 1, 1, 1);
        switch (m_State)
        {
        case AppState::Disconnected: stateStr = "Disconnected"; stateColor = ImVec4(0.6f, 0.6f, 0.6f, 1.0f); break;
        case AppState::Connecting:   stateStr = "Connecting..."; stateColor = ImVec4(1.0f, 0.8f, 0.2f, 1.0f); break;
        case AppState::Connected:    stateStr = "Connected";     stateColor = ImVec4(0.2f, 1.0f, 0.2f, 1.0f); break;
        case AppState::Testing:      stateStr = "Testing";       stateColor = ImVec4(0.4f, 0.8f, 1.0f, 1.0f); break;
        }
        ImGui::TextColored(stateColor, "State: %s", stateStr);
        ImGui::Text("Connections: %d", m_Runner.GetConnectionCount());
        ImGui::Text("Engine: %s", m_EngineMode == 0 ? "IOCP" : "RIO");
    }

    void RenderMetricsPanel(const MetricsCollector::Snapshot& snap)
    {
        // Metrics
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "REAL-TIME METRICS");
        ImGui::Separator();
        ImGui::Columns(4, "metrics", false);
        ImGui::Text("Sessions"); ImGui::Text("%u", snap.ActiveConnections); ImGui::NextColumn();
        ImGui::Text("Messages"); ImGui::Text("%llu", snap.TotalMessages); ImGui::NextColumn();
        ImGui::Text("Msg/sec"); ImGui::Text("%.0f", snap.MsgPerSec); ImGui::NextColumn();
        ImGui::Text("Avg RTT"); ImGui::Text("%.2f ms", snap.AvgLatencyMs); ImGui::NextColumn();
        ImGui::Columns(1);

        ImGui::Text("p50: %.2f ms   p95: %.2f ms   p99: %.2f ms",
            snap.P50LatencyMs, snap.P95LatencyMs, snap.P99LatencyMs);

        ImGui::Spacing();

        // Charts
        RenderCharts(m_Metrics);

        ImGui::Spacing();

        // Session Log
        RenderSessionLog();
    }

    void RenderABComparePanel()
    {
        auto snapA = m_ABCompare.GetMetricsA().GetSnapshot();
        auto snapB = m_ABCompare.GetMetricsB().GetSnapshot();

        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "A/B COMPARISON: IOCP vs RIO");
        ImGui::Separator();

        // Side-by-side metrics table
        ImGui::Columns(3, "ab_metrics", true);
        ImGui::Text("Metric"); ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.4f, 0.6f, 1.0f, 1.0f), "A (IOCP)"); ImGui::NextColumn();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.6f, 1.0f), "B (RIO)"); ImGui::NextColumn();
        ImGui::Separator();

        ImGui::Text("Sessions"); ImGui::NextColumn();
        ImGui::Text("%u", snapA.ActiveConnections); ImGui::NextColumn();
        ImGui::Text("%u", snapB.ActiveConnections); ImGui::NextColumn();

        ImGui::Text("Messages"); ImGui::NextColumn();
        ImGui::Text("%llu", snapA.TotalMessages); ImGui::NextColumn();
        ImGui::Text("%llu", snapB.TotalMessages); ImGui::NextColumn();

        ImGui::Text("Msg/sec"); ImGui::NextColumn();
        ImGui::Text("%.0f", snapA.MsgPerSec); ImGui::NextColumn();
        ImGui::Text("%.0f", snapB.MsgPerSec); ImGui::NextColumn();

        ImGui::Text("Avg RTT"); ImGui::NextColumn();
        ImGui::Text("%.2f ms", snapA.AvgLatencyMs); ImGui::NextColumn();
        ImGui::Text("%.2f ms", snapB.AvgLatencyMs); ImGui::NextColumn();

        ImGui::Text("p50"); ImGui::NextColumn();
        ImGui::Text("%.2f ms", snapA.P50LatencyMs); ImGui::NextColumn();
        ImGui::Text("%.2f ms", snapB.P50LatencyMs); ImGui::NextColumn();

        ImGui::Text("p95"); ImGui::NextColumn();
        ImGui::Text("%.2f ms", snapA.P95LatencyMs); ImGui::NextColumn();
        ImGui::Text("%.2f ms", snapB.P95LatencyMs); ImGui::NextColumn();

        ImGui::Text("p99"); ImGui::NextColumn();
        ImGui::Text("%.2f ms", snapA.P99LatencyMs); ImGui::NextColumn();
        ImGui::Text("%.2f ms", snapB.P99LatencyMs); ImGui::NextColumn();
        ImGui::Columns(1);

        ImGui::Spacing();

        // A/B Latency comparison chart
        if (ImPlot::BeginPlot("Latency Comparison (ms)", ImVec2(-1, 200)))
        {
            ImPlot::SetupAxes("Time (s)", "ms");
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, 60, ImPlotCond_Always);

            std::vector<float> latA, latB;
            m_ABCompare.GetMetricsA().GetLatencyHistory(latA);
            m_ABCompare.GetMetricsB().GetLatencyHistory(latB);

            auto plotLine = [](const char* label, const std::vector<float>& data) {
                if (!data.empty())
                {
                    std::vector<float> xs(data.size());
                    for (size_t i = 0; i < xs.size(); i++) xs[i] = static_cast<float>(i);
                    ImPlot::PlotLine(label, xs.data(), data.data(), (int)data.size());
                }
            };

            plotLine("IOCP (A)", latA);
            plotLine("RIO (B)", latB);
            ImPlot::EndPlot();
        }

        // A/B Throughput comparison chart
        if (ImPlot::BeginPlot("Throughput Comparison (msg/sec)", ImVec2(-1, 180)))
        {
            ImPlot::SetupAxes("Time (s)", "msg/sec");
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, 60, ImPlotCond_Always);

            std::vector<float> tpA, tpB;
            m_ABCompare.GetMetricsA().GetThroughputHistory(tpA);
            m_ABCompare.GetMetricsB().GetThroughputHistory(tpB);

            auto plotLine = [](const char* label, const std::vector<float>& data) {
                if (!data.empty())
                {
                    std::vector<float> xs(data.size());
                    for (size_t i = 0; i < xs.size(); i++) xs[i] = static_cast<float>(i);
                    ImPlot::PlotLine(label, xs.data(), data.data(), (int)data.size());
                }
            };

            plotLine("IOCP (A)", tpA);
            plotLine("RIO (B)", tpB);
            ImPlot::EndPlot();
        }

        ImGui::Spacing();
        RenderSessionLog();
    }

    void RenderCharts(MetricsCollector& metrics)
    {
        std::vector<float> p50Hist, p95Hist, p99Hist, tpHist;
        metrics.GetLatencyHistory(p50Hist, p95Hist, p99Hist);
        metrics.GetThroughputHistory(tpHist);

        // Plan SC: SC4 — p50/p95/p99 implot 차트 렌더링
        if (ImPlot::BeginPlot("Latency (ms)", ImVec2(-1, 180)))
        {
            ImPlot::SetupAxes("Time (s)", "ms");
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, 60, ImPlotCond_Always);

            auto plotLine = [](const char* label, const std::vector<float>& data) {
                if (!data.empty())
                {
                    std::vector<float> xs(data.size());
                    for (size_t i = 0; i < xs.size(); i++) xs[i] = static_cast<float>(i);
                    ImPlot::PlotLine(label, xs.data(), data.data(), (int)data.size());
                }
            };

            plotLine("p50", p50Hist);
            plotLine("p95", p95Hist);
            plotLine("p99", p99Hist);
            ImPlot::EndPlot();
        }

        if (ImPlot::BeginPlot("Throughput (msg/sec)", ImVec2(-1, 150)))
        {
            ImPlot::SetupAxes("Time (s)", "msg/sec");
            ImPlot::SetupAxisLimits(ImAxis_X1, 0, 60, ImPlotCond_Always);
            if (!tpHist.empty())
            {
                std::vector<float> xs(tpHist.size());
                for (size_t i = 0; i < xs.size(); i++) xs[i] = static_cast<float>(i);
                ImPlot::PlotBars("msg/sec", xs.data(), tpHist.data(), (int)tpHist.size(), 0.8f);
            }
            ImPlot::EndPlot();
        }
    }

    // Design Ref: server-status §5.1, §5.2 — Admin 탭 UI.
    // Summary 는 1Hz 폴링 (체크박스 ON 일 때). SessionList 는 Refresh 버튼 클릭 시만.
    void RenderAdminPanel()
    {
        // 폴링 주기 (1Hz) — 마지막 요청 시각 기준.
        const auto now = std::chrono::steady_clock::now();
        if (m_bAdminPolling && m_Runner.IsConnected())
        {
            if (now - m_LastAdminPoll >= std::chrono::seconds(1))
            {
                m_Runner.SendAdminSummaryRequest();
                m_LastAdminPoll = now;
            }
        }

        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "SERVER STATUS");
        ImGui::Separator();

        ImGui::Checkbox("Poll Enabled (1 Hz)", &m_bAdminPolling);
        if (!m_Runner.IsConnected())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
                "Not connected. Connect to a server first.");
            return;
        }

        ::fastport::protocols::admin::AdminStatusSummaryResponse summary;
        bool haveSummary = false;
        {
            std::lock_guard lock(m_AdminMutex);
            if (m_bAdminSummaryValid)
            {
                summary = m_AdminSummary;
                haveSummary = true;
            }
        }

        if (!haveSummary)
        {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                "Waiting for first response...");
        }
        else
        {
            // Summary 그리드
            const char* modeStr = "Unknown";
            switch (summary.server_mode())
            {
            case ::fastport::protocols::admin::SERVER_MODE_IOCP: modeStr = "IOCP"; break;
            case ::fastport::protocols::admin::SERVER_MODE_RIO:  modeStr = "RIO";  break;
            default: break;
            }
            ImGui::Text("Server Mode       : %s", modeStr);
            ImGui::Text("Uptime            : %s",  FormatUptime(summary.server_uptime_ms()).c_str());
            ImGui::Text("Active Sessions   : %u", summary.active_session_count());
            ImGui::Text("Total RX          : %s", FormatBytes(summary.total_rx_bytes()).c_str());
            ImGui::Text("Total TX          : %s", FormatBytes(summary.total_tx_bytes()).c_str());
            ImGui::Text("Idle Disconnects  : %llu",
                static_cast<unsigned long long>(summary.idle_disconnect_count()));
            ImGui::Text("Process Memory    : %s", FormatBytes(summary.process_memory_bytes()).c_str());
            ImGui::Text("Process CPU       : %.2f %%", summary.process_cpu_percent());
            ImGui::Text("Server Timestamp  : %llu ms",
                static_cast<unsigned long long>(summary.server_timestamp_ms()));
        }

        ImGui::Spacing(); ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "SESSION LIST");
        ImGui::Separator();

        ImGui::InputInt("Offset", &m_AdminOffset);
        if (m_AdminOffset < 0) m_AdminOffset = 0;
        ImGui::InputInt("Limit",  &m_AdminLimit);
        if (m_AdminLimit < 1) m_AdminLimit = 1;
        if (m_AdminLimit > 1000) m_AdminLimit = 1000;

        if (ImGui::Button("Refresh Sessions", ImVec2(200, 0)))
        {
            m_Runner.SendAdminSessionListRequest(
                static_cast<uint32_t>(m_AdminOffset),
                static_cast<uint32_t>(m_AdminLimit));
        }

        ::fastport::protocols::admin::AdminSessionListResponse listRes;
        bool haveList = false;
        {
            std::lock_guard lock(m_AdminMutex);
            if (m_bAdminSessionListValid)
            {
                listRes = m_AdminSessionList;
                haveList = true;
            }
        }

        if (haveList)
        {
            ImGui::Text("Total : %u   Offset : %u   Shown : %d",
                listRes.total(), listRes.offset(), listRes.sessions_size());

            if (ImGui::BeginTable("Sessions", 4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                ImVec2(0, 300)))
            {
                ImGui::TableSetupColumn("ID");
                ImGui::TableSetupColumn("LastRecv (ms)");
                ImGui::TableSetupColumn("RX");
                ImGui::TableSetupColumn("TX");
                ImGui::TableHeadersRow();

                for (int i = 0; i < listRes.sessions_size(); ++i)
                {
                    const auto& s = listRes.sessions(i);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0); ImGui::Text("%llu",
                        static_cast<unsigned long long>(s.session_id()));
                    ImGui::TableSetColumnIndex(1); ImGui::Text("%lld",
                        static_cast<long long>(s.last_recv_ms()));
                    ImGui::TableSetColumnIndex(2); ImGui::Text("%s",
                        FormatBytes(s.rx_bytes()).c_str());
                    ImGui::TableSetColumnIndex(3); ImGui::Text("%s",
                        FormatBytes(s.tx_bytes()).c_str());
                }
                ImGui::EndTable();
            }
        }
    }

    // Design Ref: iosession-lifetime-race §5.1~§5.3 — Stress mode UI.
    // Plan SC: FR-10 — 10k conn × 5분 × 100 churn/s reconnect 반복.
    // 이 UI 는 사용자 trigger 용 — 실제 churn loop 은 TestRunner 내부 스레드.
    void RenderStressPanel()
    {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "STRESS REPRODUCER");
        ImGui::Separator();

        // Design §7 — Stress 는 localhost 전용 도구임을 명시.
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f),
            "WARNING: Local loopback only. Do not target external servers.");
        ImGui::Spacing();

        const bool running = m_Runner.IsStressRunning();

        // Inputs — running 중엔 비활성.
        ImGui::BeginDisabled(running);
        {
            ImGui::InputText("Target IP",   m_StressIP, sizeof(m_StressIP));
            ImGui::InputInt ("Target Port", &m_StressPort);
            if (m_StressPort < 1)     m_StressPort = 1;
            if (m_StressPort > 65535) m_StressPort = 65535;

            // Design Ref: iosession-lifetime-race v0.2 §5 — Scenario A/B/C 라디오.
            ImGui::Text("Scenario:");
            ImGui::SameLine(); ImGui::RadioButton("A Churn",    &m_StressScenario, 0);
            ImGui::SameLine(); ImGui::RadioButton("B Burst",    &m_StressScenario, 1);
            ImGui::SameLine(); ImGui::RadioButton("C Combined", &m_StressScenario, 2);
            ImGui::Separator();

            if (m_StressScenario == 0)
            {
                // Scenario A — Churn
                ImGui::TextDisabled("A: %d conn × %d churn/s × %d sec",
                    m_StressTargetConns, m_StressChurnRate, m_StressDuration);
                ImGui::InputInt("Target Conns", &m_StressTargetConns);
                if (m_StressTargetConns < 1)    m_StressTargetConns = 1;
                if (m_StressTargetConns > 50000) m_StressTargetConns = 50000;
                ImGui::InputInt("Churn rate /sec", &m_StressChurnRate);
                if (m_StressChurnRate < 0)    m_StressChurnRate = 0;
                if (m_StressChurnRate > 1000) m_StressChurnRate = 1000;
                ImGui::InputInt("Duration (sec)", &m_StressDuration);
                if (m_StressDuration < 10)   m_StressDuration = 10;
                if (m_StressDuration > 3600) m_StressDuration = 3600;
            }
            else if (m_StressScenario == 1)
            {
                // Scenario B — Burst (1M × 2 round)
                ImGui::TextDisabled("B: %d session(s) × %d packet × %d round",
                    m_StressBurstSessions, m_StressBurstPackets, m_StressBurstRounds);
                ImGui::InputInt("Concurrent Sessions (1~8)", &m_StressBurstSessions);
                if (m_StressBurstSessions < 1) m_StressBurstSessions = 1;
                if (m_StressBurstSessions > 8) m_StressBurstSessions = 8;
                ImGui::InputInt("Packets per Round", &m_StressBurstPackets);
                if (m_StressBurstPackets < 1000)    m_StressBurstPackets = 1000;
                if (m_StressBurstPackets > 10000000) m_StressBurstPackets = 10000000;
                if (ImGui::SmallButton("100k"))  m_StressBurstPackets = 100000;
                ImGui::SameLine();
                if (ImGui::SmallButton("1M"))    m_StressBurstPackets = 1000000;
                ImGui::SameLine();
                if (ImGui::SmallButton("10M"))   m_StressBurstPackets = 10000000;
                ImGui::InputInt("Round Count", &m_StressBurstRounds);
                if (m_StressBurstRounds < 1)  m_StressBurstRounds = 1;
                if (m_StressBurstRounds > 10) m_StressBurstRounds = 10;
            }
            else
            {
                // Scenario C — Combined
                ImGui::TextDisabled("C: %d conn × ~%d pps × %d sec",
                    m_StressCombinedConns, m_StressCombinedPps, m_StressDuration);
                ImGui::InputInt("Connection Count", &m_StressCombinedConns);
                if (m_StressCombinedConns < 1)    m_StressCombinedConns = 1;
                if (m_StressCombinedConns > 10000) m_StressCombinedConns = 10000;
                ImGui::InputInt("PPS per Session (ref)", &m_StressCombinedPps);
                if (m_StressCombinedPps < 1)    m_StressCombinedPps = 1;
                if (m_StressCombinedPps > 1000) m_StressCombinedPps = 1000;
                ImGui::InputInt("Duration (sec)", &m_StressDuration);
                if (m_StressDuration < 10)   m_StressDuration = 10;
                if (m_StressDuration > 3600) m_StressDuration = 3600;
            }

            ImGui::Separator();
            // Server mode: v1 IOCP only. RIO 는 freeze (v1.1 이관).
            ImGui::RadioButton("IOCP (v1)", &m_StressServerMode, 0); ImGui::SameLine();
            ImGui::BeginDisabled(true);
            ImGui::RadioButton("RIO (v1.1)", &m_StressServerMode, 1);
            ImGui::EndDisabled();
            ImGui::TextDisabled("(UI label — server is a separate process; RIO frozen for v1)");

            ImGui::Spacing();
            // Design Ref: iosession-lifetime-race §5.2 — Echo 트래픽 옵션.
            ImGui::Checkbox("Enable Echo Ping-Pong", &m_StressEnableEcho);
            ImGui::InputInt("Payload Size (bytes)", &m_StressPayloadBytes);
            if (m_StressPayloadBytes < 1)    m_StressPayloadBytes = 1;
            if (m_StressPayloadBytes > 8192) m_StressPayloadBytes = 8192;
            if (ImGui::SmallButton("4 B"))    m_StressPayloadBytes = 4;
            ImGui::SameLine();
            if (ImGui::SmallButton("64 B"))   m_StressPayloadBytes = 64;
            ImGui::SameLine();
            if (ImGui::SmallButton("1 KB"))   m_StressPayloadBytes = 1024;
            ImGui::SameLine();
            if (ImGui::SmallButton("4 KB"))   m_StressPayloadBytes = 4096;
        }
        ImGui::EndDisabled();

        ImGui::Spacing();

        if (!running)
        {
            if (ImGui::Button("Start Stress", ImVec2(180, 0)))
            {
                TestRunner::StressConfig cfg;
                cfg.scenario = static_cast<TestRunner::StressScenario>(m_StressScenario);
                cfg.durationSec  = m_StressDuration;
                cfg.enableEcho   = m_StressEnableEcho;
                cfg.payloadBytes = m_StressPayloadBytes;
                cfg.targetConns     = m_StressTargetConns;
                cfg.churnRatePerSec = m_StressChurnRate;
                cfg.burstConcurrentSessions = m_StressBurstSessions;
                cfg.burstPacketCount        = m_StressBurstPackets;
                cfg.burstRoundCount         = m_StressBurstRounds;
                cfg.combinedConnCount     = m_StressCombinedConns;
                cfg.combinedPpsPerSession = m_StressCombinedPps;

                if (m_Runner.StartStressMode(m_StressIP, m_StressPort, cfg))
                {
                    char buf[200];
                    const char* scenarioLabel =
                        (m_StressScenario == 0) ? "A(Churn)" :
                        (m_StressScenario == 1) ? "B(Burst)" : "C(Combined)";
                    if (m_StressScenario == 1)
                    {
                        snprintf(buf, sizeof(buf),
                            "Stress started: %s — %d session(s) × %d packet × %d round, echo=%s (payload %d B)",
                            scenarioLabel, m_StressBurstSessions, m_StressBurstPackets, m_StressBurstRounds,
                            cfg.enableEcho ? "on" : "off", cfg.payloadBytes);
                    }
                    else if (m_StressScenario == 2)
                    {
                        snprintf(buf, sizeof(buf),
                            "Stress started: %s — %d conn × ~%d pps × %d sec, echo=%s (payload %d B)",
                            scenarioLabel, m_StressCombinedConns, m_StressCombinedPps, m_StressDuration,
                            cfg.enableEcho ? "on" : "off", cfg.payloadBytes);
                    }
                    else
                    {
                        snprintf(buf, sizeof(buf),
                            "Stress started: %s — %d conns, %d churn/s, %d sec, echo=%s (payload %d B)",
                            scenarioLabel, cfg.targetConns, cfg.churnRatePerSec, cfg.durationSec,
                            cfg.enableEcho ? "on" : "off", cfg.payloadBytes);
                    }
                    Log(buf);
                }
                else { Log("Stress start failed (already running?)"); }
            }
        }
        else
        {
            if (ImGui::Button("Stop", ImVec2(180, 0)))
            {
                m_Runner.StopStressMode();
                Log("Stress stopped.");
            }
        }

        ImGui::Spacing(); ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "LIVE STATS");
        ImGui::Separator();

        // Design §5.3 체크리스트 7 stat fields.
        const auto& st = m_Runner.GetStressStats();
        const auto attempts = st.connectAttempts.load(std::memory_order_relaxed);
        const auto failures = st.connectFailures.load(std::memory_order_relaxed);
        const auto accepted = st.totalAccepted.load(std::memory_order_relaxed);
        const auto churned  = st.churned.load(std::memory_order_relaxed);
        const int  active   = st.active.load(std::memory_order_relaxed);
        const int  elapsed  = st.elapsedSec.load(std::memory_order_relaxed);
        const bool crash    = st.crashSuspected.load(std::memory_order_relaxed);

        ImGui::Text("Connect Attempts : %llu", static_cast<unsigned long long>(attempts));
        ImGui::Text("Connect Failures : %llu", static_cast<unsigned long long>(failures));
        ImGui::Text("Total Accepted   : %llu", static_cast<unsigned long long>(accepted));
        ImGui::Text("Active           : %d",   active);

        if (m_StressScenario == 1)
        {
            // Scenario B — Burst: round + packetsReceived.
            const int round = st.currentRound.load(std::memory_order_relaxed);
            const auto pkts = st.packetsReceived.load(std::memory_order_relaxed);
            ImGui::Text("Current Round    : %d / %d", round, m_StressBurstRounds);
            ImGui::Text("Packets Received : %llu", static_cast<unsigned long long>(pkts));
            ImGui::Text("Elapsed          : %d sec", elapsed);
        }
        else if (m_StressScenario == 2)
        {
            // Scenario C — Combined: packetsReceived + elapsed / duration.
            const auto pkts = st.packetsReceived.load(std::memory_order_relaxed);
            ImGui::Text("Packets Received : %llu", static_cast<unsigned long long>(pkts));
            ImGui::Text("Elapsed          : %d / %d sec", elapsed, m_StressDuration);
        }
        else
        {
            // Scenario A — Churn.
            ImGui::Text("Churned          : %llu", static_cast<unsigned long long>(churned));
            ImGui::Text("Elapsed          : %d / %d sec", elapsed, m_StressDuration);
        }
        if (crash)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                "Crash detected   : Yes (failure rate > 50%%)");
        }
        else
        {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                "Crash detected   : None");
        }

        ImGui::Spacing(); ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "LOG TAIL (last 5)");
        ImGui::Separator();
        {
            std::lock_guard lock(m_LogMutex);
            const int total = static_cast<int>(m_LogMessages.size());
            const int start = (total > 5) ? (total - 5) : 0;
            for (int i = start; i < total; ++i)
            {
                ImGui::TextUnformatted(m_LogMessages[i].c_str());
            }
            if (total == 0)
            {
                ImGui::TextDisabled("(no log yet)");
            }
        }
    }

    // bytes 자동 단위 포맷 (B/KB/MB/GB).
    static std::string FormatBytes(uint64_t bytes)
    {
        const char* units[] = { "B", "KB", "MB", "GB", "TB" };
        int idx = 0;
        double v = static_cast<double>(bytes);
        while (v >= 1024.0 && idx < 4)
        {
            v /= 1024.0;
            ++idx;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "%.2f %s", v, units[idx]);
        return buf;
    }

    // ms → HH:MM:SS 포맷.
    static std::string FormatUptime(uint64_t ms)
    {
        const uint64_t totalSec = ms / 1000ULL;
        const uint64_t h = totalSec / 3600ULL;
        const uint64_t m = (totalSec % 3600ULL) / 60ULL;
        const uint64_t s = totalSec % 60ULL;
        char buf[32];
        snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu",
            static_cast<unsigned long long>(h),
            static_cast<unsigned long long>(m),
            static_cast<unsigned long long>(s));
        return buf;
    }

    void RenderSessionLog()
    {
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "SESSION LOG");
        ImGui::Separator();
        ImGui::BeginChild("LogRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard lock(m_LogMutex);
            for (auto& line : m_LogMessages)
            {
                ImGui::TextUnformatted(line.c_str());
            }
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    }

    void Log(const char* msg)
    {
        {
            std::lock_guard lock(m_LogMutex);
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
            localtime_s(&tm, &t);
            char buf[256];
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s", tm.tm_hour, tm.tm_min, tm.tm_sec, msg);
            m_LogMessages.push_back(buf);
            if (m_LogMessages.size() > MAX_LOG_LINES) m_LogMessages.pop_front();
        }
        if (m_LogCallback) m_LogCallback(msg);
    }

    // App state
    AppState m_State = AppState::Disconnected;
    char m_ServerIP[64] = "127.0.0.1";
    int m_ServerPort = 6628;
    int m_EngineMode = 0;  // 0=IOCP, 1=RIO (label only, client always uses IOCP)
    int m_EchoCount = 100;

    // A/B Compare state
    char m_ABIpA[64] = "127.0.0.1";
    int m_ABPortA = 6628;
    char m_ABIpB[64] = "127.0.0.1";
    int m_ABPortB = 6629;
    int m_ABEchoCount = 100;
    bool m_bABMode = false;

    // Modules
    MetricsCollector m_Metrics;
    TestRunner m_Runner;
    ABCompare m_ABCompare;

    // Log
    LogCallback m_LogCallback;
    std::mutex m_LogMutex;
    std::deque<std::string> m_LogMessages;
    static constexpr size_t MAX_LOG_LINES = 200;

    // Design Ref: server-status §5.2 — Admin 탭 상태.
    bool                                                     m_bAdminPolling { true };
    std::chrono::steady_clock::time_point                    m_LastAdminPoll {};
    int                                                      m_AdminOffset   { 0 };
    int                                                      m_AdminLimit    { 100 };
    std::mutex                                               m_AdminMutex;
    bool                                                     m_bAdminSummaryValid     { false };
    bool                                                     m_bAdminSessionListValid { false };
    ::fastport::protocols::admin::AdminStatusSummaryResponse m_AdminSummary;
    ::fastport::protocols::admin::AdminSessionListResponse   m_AdminSessionList;

    // Design Ref: iosession-lifetime-race §5.1, §5.3 — Stress 탭 상태.
    // 기본값은 Design §8.5 / Plan Q10 의 최종 stress 사양(10k / 100 churn/s / 300s).
    // 사용자는 smoke test 시 작은 값으로 조정 가능.
    char                                                     m_StressIP[64]         = "127.0.0.1";
    int                                                      m_StressPort           = 6628;
    // Scenario 선택 (v0.2): 0=A Churn, 1=B Burst, 2=C Combined.
    int                                                      m_StressScenario       = 0;
    // Common
    int                                                      m_StressDuration       = 300;
    // A / Churn
    int                                                      m_StressTargetConns    = 10000;
    int                                                      m_StressChurnRate      = 100;
    // B / Burst
    int                                                      m_StressBurstSessions  = 1;
    int                                                      m_StressBurstPackets   = 1000000;
    int                                                      m_StressBurstRounds    = 2;
    // C / Combined
    int                                                      m_StressCombinedConns  = 1000;
    int                                                      m_StressCombinedPps    = 100;
    // Common echo
    int                                                      m_StressServerMode     = 0;   // 0=IOCP only (v1). RIO freeze.
    bool                                                     m_StressEnableEcho     = true;
    int                                                      m_StressPayloadBytes   = 4;
};
