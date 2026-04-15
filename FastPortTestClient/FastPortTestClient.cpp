// FastPortTestClient.cpp
// Design Ref: §3.1 — ImGui + DX11 Entry Point (Option C Pragmatic Balance)
// Plan SC: SC1 (ImGui 창에서 서버 연결), SC2 (실시간 차트)

#include <Windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <cstdio>
#include <filesystem>
#include <spdlog/spdlog.h>

#include <imgui.h>
#include <implot.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

import networks.core.socket;
import commons.logger;
import commons.event_listener;
import test_client.metrics_collector;
import test_client.test_runner;

// DX11 globals
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;

// Forward declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Session log
static std::mutex g_LogMutex;
static std::deque<std::string> g_LogMessages;
static constexpr size_t MAX_LOG_LINES = 200;

void AddLog(const char* msg)
{
    std::lock_guard lock(g_LogMutex);
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[256];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s", tm.tm_hour, tm.tm_min, tm.tm_sec, msg);
    g_LogMessages.push_back(buf);
    if (g_LogMessages.size() > MAX_LOG_LINES) g_LogMessages.pop_front();
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
    // 로거 먼저 초기화 (Socket::Initialize가 내부적으로 로거 사용)
    auto& logger = LibCommons::Logger::GetInstance();
    std::string location = std::filesystem::current_path().string();
    logger.Create(location + "/loggers", "test_client.txt", 1024 * 1024 * 10, 3, false);

    // 네트워크 초기화
    LibNetworks::Core::Socket::Initialize();
    LibCommons::EventListener::GetInstance().Init(std::thread::hardware_concurrency());

    // Win32 윈도우 생성
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                       GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
                       L"FastPortTestClient", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"FastPort Test Client",
                                WS_OVERLAPPEDWINDOW, 100, 100, 1280, 720,
                                nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // App state
    MetricsCollector metrics;
    TestRunner runner;
    runner.SetMetrics(&metrics);
    runner.SetLogCallback(AddLog);

    static char serverIP[64] = "127.0.0.1";
    static int serverPort = 6628;
    static int engineMode = 0;
    static int echoCount = 100;

    ImVec4 clear_color = ImVec4(0.06f, 0.06f, 0.08f, 1.0f);
    bool done = false;

    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // ===== Main UI =====
        auto snap = metrics.GetSnapshot();

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2((float)io.DisplaySize.x, (float)io.DisplaySize.y), ImGuiCond_FirstUseEver);
        ImGui::Begin("FastPort Test Client", nullptr, ImGuiWindowFlags_NoCollapse);

        // Left panel: Controls
        ImGui::BeginChild("Controls", ImVec2(250, 0), true);
        {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "CONNECTION");
            ImGui::Separator();
            ImGui::InputText("Server", serverIP, sizeof(serverIP));
            ImGui::InputInt("Port", &serverPort);
            ImGui::RadioButton("IOCP", &engineMode, 0); ImGui::SameLine();
            ImGui::RadioButton("RIO", &engineMode, 1);

            if (!runner.IsConnected())
            {
                if (ImGui::Button("Connect", ImVec2(-1, 0)))
                {
                    AddLog(engineMode == 0 ? "Connecting (IOCP)..." : "Connecting (RIO)...");
                    if (runner.Connect(serverIP, serverPort))
                        AddLog("Connection initiated");
                    else
                        AddLog("Connection failed!");
                }
            }
            else
            {
                if (ImGui::Button("Disconnect", ImVec2(-1, 0)))
                {
                    runner.Disconnect();
                    metrics.Reset();
                    AddLog("Disconnected");
                }
            }

            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "TESTS");
            ImGui::Separator();

            ImGui::InputInt("Echo Count", &echoCount);
            if (ImGui::Button("Run Echo Test", ImVec2(-1, 0)))
            {
                char buf[64];
                snprintf(buf, sizeof(buf), "Echo test: %d messages", echoCount);
                AddLog(buf);
                // TODO: wire to TestSession::StartEchoLoop when sessions are accessible
            }

            ImGui::Spacing();
            ImGui::Text("Scale Test:");
            if (ImGui::Button("1", ImVec2(50, 0))) { runner.ConnectScale(serverIP, serverPort, 1); AddLog("Scale: +1"); }
            ImGui::SameLine();
            if (ImGui::Button("10", ImVec2(50, 0))) { runner.ConnectScale(serverIP, serverPort, 10); AddLog("Scale: +10"); }
            ImGui::SameLine();
            if (ImGui::Button("100", ImVec2(50, 0))) { runner.ConnectScale(serverIP, serverPort, 100); AddLog("Scale: +100"); }

            ImGui::Spacing(); ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "STATUS");
            ImGui::Separator();
            ImGui::Text("Connections: %d", runner.GetConnectionCount());
            ImGui::Text("Engine: %s", engineMode == 0 ? "IOCP" : "RIO");
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Right panel: Metrics + Charts + Log
        ImGui::BeginChild("Main", ImVec2(0, 0), false);
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

            // Latency Chart
            std::vector<float> latHist, tpHist;
            metrics.GetLatencyHistory(latHist);
            metrics.GetThroughputHistory(tpHist);

            if (ImPlot::BeginPlot("Latency (ms)", ImVec2(-1, 180)))
            {
                ImPlot::SetupAxes("Time (s)", "ms");
                ImPlot::SetupAxisLimits(ImAxis_X1, 0, 60, ImPlotCond_Always);
                if (!latHist.empty())
                {
                    std::vector<float> xs(latHist.size());
                    for (size_t i = 0; i < xs.size(); i++) xs[i] = static_cast<float>(i);
                    ImPlot::PlotLine("p50", xs.data(), latHist.data(), (int)latHist.size());
                }
                ImPlot::EndPlot();
            }

            // Throughput Chart
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

            ImGui::Spacing();

            // Session Log
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "SESSION LOG");
            ImGui::Separator();
            ImGui::BeginChild("LogRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
            {
                std::lock_guard lock(g_LogMutex);
                for (auto& line : g_LogMessages)
                {
                    ImGui::TextUnformatted(line.c_str());
                }
                if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
                    ImGui::SetScrollHereY(1.0f);
            }
            ImGui::EndChild();
        }
        ImGui::EndChild();

        ImGui::End();

        // Render
        ImGui::Render();
        const float cc[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w,
                               clear_color.z * clear_color.w, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, cc);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    // Cleanup
    runner.Disconnect();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

// DX11 helpers
bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate = { 60, 1 };
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc = { 1, 0 };
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL fls[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        fls, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            fls, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (hr != S_OK) return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBuf = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBuf));
    if (pBuf) { g_pd3dDevice->CreateRenderTargetView(pBuf, nullptr, &g_mainRenderTargetView); pBuf->Release(); }
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
