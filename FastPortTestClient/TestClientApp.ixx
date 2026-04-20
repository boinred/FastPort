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
};
