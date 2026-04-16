# test-client-ui Design Document

> **Summary**: ImGui + implot 기반 FastPort 듀얼 엔진 GUI 테스트 클라이언트
>
> **Project**: FastPort
> **Author**: AnYounggun
> **Date**: 2026-04-15
> **Status**: Draft
> **Planning Doc**: [test-client-ui.plan.md](../../docs/01-plan/features/test-client-ui.plan.md)
> **Selected Architecture**: Option C — Pragmatic Balance

---

## Context Anchor

| Key | Value |
|-----|-------|
| **WHY** | 콘솔 로그/CSV만으로는 엔진 동작 검증과 팀 설득이 어려움 |
| **WHO** | FastPort 개발팀 (검증), 사내 기술 의사결정자 (데모) |
| **RISK** | ImGui + C++20 모듈 통합 호환성, 멀티 연결 시 GUI 스레드 블로킹 |
| **SUCCESS** | IOCP/RIO 양 모드 연결 + 실시간 차트 + A/B 비교 동작 |
| **SCOPE** | Phase 1: 기본 연결/에코/메트릭, Phase 2: A/B 비교 + 스케일 테스트 |

---

## 1. Overview

### 1.1 Design Goals

1. **GUI/IO 완전 분리** — ImGui 렌더링은 메인 스레드, 네트워크 I/O는 별도 스레드
2. **기존 엔진 재사용** — LibNetworks의 IOService, IOSocketConnector, IOSession 그대로 사용
3. **최소 파일 구조** — 6개 파일로 전체 기능 커버, 폴더 분리 없이 평탄한 구조
4. **데모 친화적** — 3분 내 IOCP vs RIO 성능 차이를 시각적으로 보여줄 수 있는 UX

### 1.2 Design Principles

- GUI 스레드에서 절대 블로킹 I/O 호출하지 않음
- 메트릭 전달은 atomic 변수/lock-free 구조로 (락 최소화)
- 기존 FastPortServer 코드 수정 없이 동작

---

## 2. Architecture

### 2.1 Selected Option: C — Pragmatic Balance

```
FastPortTestClient/
├── FastPortTestClient.vcxproj
├── FastPortTestClient.cpp    # main() + DX11/ImGui 초기화 + 렌더 루프
├── TestClientApp.ixx         # UI 레이아웃 + 앱 상태 관리
├── TestSession.ixx           # OutboundSession 확장 + RTT 측정
├── MetricsCollector.ixx      # atomic 카운터 + p50/p95/p99 계산
├── TestRunner.ixx            # 에코/플러드/스케일 테스트 로직
└── ABCompare.ixx             # A/B 비교 모드 (2개 서버 동시 테스트)
```

### 2.2 Thread Model

```
┌─ Main Thread ──────────────────────────────┐
│                                            │
│  Win32 Message Pump                        │
│  ┌──────────────────────────────────────┐  │
│  │ DX11 Frame Loop (~60fps)            │  │
│  │  1. ImGui::NewFrame()               │  │
│  │  2. TestClientApp::Render()         │  │
│  │     ├─ Controls panel               │  │
│  │     ├─ Metrics panel (read atomics) │  │
│  │     ├─ Charts (implot)              │  │
│  │     └─ Session log                  │  │
│  │  3. ImGui::Render()                 │  │
│  │  4. DX11 Present                    │  │
│  └──────────────────────────────────────┘  │
│                                            │
│  Commands → TestRunner (start/stop tests)  │
└────────────────┬───────────────────────────┘
                 │ (lock-free command queue)
┌─ IO Thread(s) ─┴───────────────────────────┐
│                                            │
│  IOService (IOCP worker pool)              │
│  ┌──────────────────────────────────────┐  │
│  │ TestSession(s)                       │  │
│  │  ├─ OnPacketReceived()              │  │
│  │  │   └─ RTT 계산 → MetricsCollector │  │
│  │  └─ OnDisconnected()               │  │
│  └──────────────────────────────────────┘  │
│                                            │
│  MetricsCollector (atomic updates)         │
│  ├─ m_TotalMessages.fetch_add(1)          │
│  ├─ m_LatencySamples.push(rtt)            │
│  └─ 매 1초 p50/p95/p99 재계산             │
└────────────────────────────────────────────┘
```

### 2.3 Data Flow

```
GUI Thread                    IO Thread(s)
    │                             │
    │  "Connect IOCP:6628"        │
    ├────────────────────────────►│ IOSocketConnector::Create()
    │                             │ → TestSession created
    │                             │
    │  "Run Echo Test x100"       │
    ├────────────────────────────►│ TestRunner::RunEcho(100)
    │                             │ → SendMessage() x100
    │                             │
    │  (매 프레임)                 │ OnPacketReceived()
    │  MetricsCollector::         │ → RTT 계산
    │    GetSnapshot()            │ → MetricsCollector::
    │◄────────────────────────────┤   RecordLatency(rtt)
    │  (atomic read, 락 없음)      │   RecordMessage()
    │                             │
    │  implot::PlotLine()         │
    │  (차트 렌더링)               │
```

---

## 3. Module Details

### 3.1 FastPortTestClient.cpp — Entry Point

```
책임:
- Win32 윈도우 생성
- DX11 디바이스/스왑체인 초기화
- ImGui + implot 컨텍스트 초기화
- 메인 렌더 루프 (WM_QUIT까지)
- 정리 (ImGui shutdown, DX11 release)

참고: ImGui DX11 예제(imgui_impl_dx11.h/cpp) 그대로 사용
```

### 3.2 TestClientApp.ixx — UI + 앱 상태

```
export class TestClientApp
{
public:
    void Initialize();
    void Render();      // 매 프레임 호출
    void Shutdown();

private:
    // 앱 상태
    enum class AppState { Disconnected, Connecting, Connected, Testing };
    AppState m_State = AppState::Disconnected;

    // 연결 설정
    char m_ServerIP[64] = "127.0.0.1";
    int m_ServerPort = 6628;
    int m_EngineMode = 0;  // 0=IOCP, 1=RIO

    // 모듈 참조
    std::unique_ptr<TestRunner> m_pTestRunner;
    std::unique_ptr<MetricsCollector> m_pMetrics;
    std::unique_ptr<ABCompare> m_pABCompare;

    // UI 렌더링
    void RenderControlsPanel();
    void RenderMetricsPanel();
    void RenderChartsPanel();
    void RenderSessionLog();
    void RenderABComparePanel();
};
```

### 3.3 TestSession.ixx — 네트워크 세션 + RTT 측정

```
export class TestSession : public LibNetworks::Sessions::OutboundSession
{
public:
    // OutboundSession 확장
    // 에코 전송 시 타임스탬프 기록 → 수신 시 RTT 계산

protected:
    void OnPacketReceived(const Core::Packet& packet) override;
    void OnConnected() override;
    void OnDisconnected() override;

private:
    MetricsCollector* m_pMetrics = nullptr;  // 약한 참조
    std::chrono::steady_clock::time_point m_LastSendTime;
};
```

### 3.4 MetricsCollector.ixx — 통계 수집

```
export class MetricsCollector
{
public:
    // IO 스레드에서 호출 (atomic)
    void RecordLatency(double rttMs);
    void RecordMessage();
    void RecordConnection(bool connected);

    // GUI 스레드에서 호출 (스냅샷 복사)
    struct Snapshot {
        uint64_t totalMessages;
        uint32_t activeConnections;
        double msgPerSec;
        double avgLatencyMs;
        double p50LatencyMs;
        double p95LatencyMs;
        double p99LatencyMs;
    };
    Snapshot GetSnapshot() const;

    // 차트 데이터 (롤링 60초)
    const std::vector<float>& GetLatencyHistory() const;
    const std::vector<float>& GetThroughputHistory() const;

private:
    // atomic 카운터
    std::atomic<uint64_t> m_TotalMessages = 0;
    std::atomic<uint32_t> m_ActiveConnections = 0;

    // 레이턴시 샘플 (lock-free ring buffer)
    static constexpr size_t MAX_SAMPLES = 10000;
    std::array<float, MAX_SAMPLES> m_LatencySamples;
    std::atomic<size_t> m_SampleHead = 0;

    // 1초 주기 통계 (GUI 스레드에서 갱신)
    mutable std::mutex m_HistoryMutex;
    std::vector<float> m_LatencyHistory;   // 롤링 60개 (1초 간격)
    std::vector<float> m_ThroughputHistory;
};
```

### 3.5 TestRunner.ixx — 테스트 실행기

```
export class TestRunner
{
public:
    void SetMetrics(MetricsCollector* pMetrics);

    // 연결 관리
    bool Connect(const char* ip, int port, bool bRioMode);
    void Disconnect();
    bool IsConnected() const;

    // 테스트
    void RunEchoTest(int count);         // 에코 N회
    void RunFloodTest(int durationSec);  // N초간 최대 속도 전송
    void RunScaleTest(int connections);  // N개 동시 연결 생성

    // 상태
    bool IsTestRunning() const;
    void StopTest();

private:
    std::shared_ptr<LibNetworks::Services::IOService> m_pIOService;
    std::vector<std::shared_ptr<TestSession>> m_Sessions;
    std::atomic<bool> m_bTestRunning = false;
};
```

### 3.6 ABCompare.ixx — A/B 비교 모드

```
export class ABCompare
{
public:
    // 2개 서버에 동시 연결
    bool Start(const char* ipA, int portA,    // IOCP 서버
               const char* ipB, int portB);   // RIO 서버

    // 동일 워크로드 동시 수행
    void RunParallelEcho(int count);
    void RunParallelScale(int connections);

    // 결과 (각각의 MetricsCollector)
    MetricsCollector& GetMetricsA();  // IOCP
    MetricsCollector& GetMetricsB();  // RIO

    void Stop();

private:
    std::unique_ptr<TestRunner> m_pRunnerA;  // IOCP
    std::unique_ptr<TestRunner> m_pRunnerB;  // RIO
    MetricsCollector m_MetricsA;
    MetricsCollector m_MetricsB;
};
```

---

## 4. UI Layout

```
┌─────────────────────────────────────────────────────────┐
│  FastPort Test Client v1.0              [IOCP] [RIO]    │
├──────────────┬──────────────────────────────────────────┤
│              │                                          │
│  CONNECTION  │  REAL-TIME METRICS                       │
│  ──────────  │  ────────────────                        │
│  Server:     │  Sessions:    0    Messages:   0         │
│  [127.0.0.1] │  Msg/sec:     0    Avg RTT:    -         │
│  Port:       │  p50:         -    p95:        -         │
│  [6628    ]  │  p99:         -                          │
│  Mode:       │                                          │
│  (•)IOCP     │  LATENCY CHART (60s rolling)             │
│  ( )RIO      │  ┌────────────────────────────────────┐  │
│              │  │     ╱╲    ╱╲                        │  │
│  [Connect]   │  │    ╱  ╲╱╱  ╲     p50 ── p95 ──    │  │
│  [Disconnect]│  │   ╱        ╲    p99 ──             │  │
│              │  └────────────────────────────────────┘  │
│  TESTS       │                                          │
│  ──────────  │  THROUGHPUT CHART                        │
│  Echo:       │  ┌────────────────────────────────────┐  │
│  Count:[100] │  │ ████████████  45,678 msg/sec       │  │
│  [Run Echo]  │  └────────────────────────────────────┘  │
│              │                                          │
│  Scale:      │  SESSION LOG                             │
│  [1][10]     │  ┌────────────────────────────────────┐  │
│  [100][1000] │  │ 08:23:01 Connected (IOCP)          │  │
│              │  │ 08:23:01 Echo #1: 0.12ms           │  │
│  [Flood 10s] │  │ 08:23:01 Echo #2: 0.15ms           │  │
│              │  │ 08:23:02 Echo #3: 0.11ms           │  │
│  A/B COMPARE │  └────────────────────────────────────┘  │
│  ──────────  │                                          │
│  IOCP: 6628  │                                          │
│  RIO:  6629  │                                          │
│  [Start A/B] │                                          │
│              │                                          │
└──────────────┴──────────────────────────────────────────┘
```

---

## 5. Dependencies

### 5.1 vcpkg 추가 패키지

```json
// vcpkg.json에 추가
"imgui[docking-experimental,dx11-binding,win32-binding]",
"implot"
```

### 5.2 프로젝트 참조

| 참조 | 용도 |
|------|------|
| LibNetworks.lib | IOService, IOSocketConnector, IOSession, Socket |
| LibCommons.lib | Logger, CircleBufferQueue, EventListener |
| Protocols.lib | protobuf 메시지 (에코 테스트용) |
| d3d11.lib | DX11 렌더링 |
| dxgi.lib | 스왑체인 |

---

## 6. Key Implementation Details

### 6.1 ImGui + C++20 모듈 통합 전략

ImGui는 C 스타일 헤더이므로 C++20 모듈과 직접 혼용 어려움.
해결: `FastPortTestClient.cpp` (비모듈 .cpp)에서 ImGui 헤더 포함, 모듈 파일(.ixx)에서는 ImGui를 직접 include하지 않고 인터페이스를 통해 간접 사용.

```cpp
// FastPortTestClient.cpp (비모듈)
#include <imgui.h>
#include <implot.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

import test_client_app;  // 모듈

// 메인 루프에서:
TestClientApp app;
while (running) {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    app.Render();  // 모듈 함수 호출

    ImGui::Render();
    // DX11 present...
}
```

```cpp
// TestClientApp.ixx (모듈)
// ImGui 헤더를 직접 include하지 않음
// 대신 ImGui 함수는 extern 선언 또는 콜백으로 접근
// 또는: module; 블록에서 #include <imgui.h> (글로벌 모듈 프래그먼트)
module;
#include <imgui.h>    // 글로벌 모듈 프래그먼트에서 가능
#include <implot.h>
export module test_client_app;
```

### 6.2 RTT 측정 방식

```cpp
// 에코 전송 시:
auto sendTime = std::chrono::steady_clock::now();
// 패킷 ID에 타임스탬프 인코딩 (또는 맵에 저장)

// 에코 수신 시:
auto recvTime = std::chrono::steady_clock::now();
double rttMs = std::chrono::duration<double, std::milli>(recvTime - sendTime).count();
m_pMetrics->RecordLatency(rttMs);
```

### 6.3 Percentile 계산

```cpp
// 매 1초 주기로 수집된 샘플을 정렬하여 계산
void MetricsCollector::UpdatePercentiles() {
    std::vector<float> sorted(samples.begin(), samples.end());
    std::sort(sorted.begin(), sorted.end());
    size_t n = sorted.size();
    m_P50 = sorted[n * 50 / 100];
    m_P95 = sorted[n * 95 / 100];
    m_P99 = sorted[n * 99 / 100];
}
```

---

## 7. Error Handling

| Scenario | Handling |
|----------|----------|
| 서버 연결 실패 | 상태를 Disconnected로, 로그에 에러 표시, 연결 버튼 재활성화 |
| 테스트 중 연결 끊김 | 진행 중인 테스트 중단, 메트릭 보존, 로그에 표시 |
| ImGui 렌더링 에러 | DX11 디바이스 로스트 시 자동 재생성 |
| 1000개 연결 시 포트 고갈 | 에러 카운터 표시, 실패한 연결 수 로그 |

---

## 8. Test Plan

| # | Test Case | Type | Expected |
|---|-----------|------|----------|
| T1 | IOCP 모드 단일 연결 에코 | Manual | RTT 표시, 차트 갱신 |
| T2 | RIO 모드 단일 연결 에코 | Manual | RTT 표시, 차트 갱신 |
| T3 | 100개 동시 연결 스케일 | Manual | 연결 수 표시, msg/sec 차트 |
| T4 | A/B 비교 모드 | Manual | 양쪽 메트릭 나란히 표시 |
| T5 | 서버 다운 시 재연결 동작 | Manual | Disconnected 상태로 전환 |
| T6 | 30분 연속 플러드 테스트 | Stress | 메모리 누수 없음 |

---

## 9. Risks Revisited

| Risk | Mitigation (Design-level) |
|------|--------------------------|
| ImGui + C++20 모듈 | 글로벌 모듈 프래그먼트(`module;` 블록)에서 ImGui 헤더 include |
| GUI 블로킹 | IOService가 별도 스레드 풀에서 동작, GUI는 atomic read만 |
| 대량 연결 메모리 | TestSession당 ~16KB (기존 CircleBufferQueue), 1000개 = ~16MB |

---

## 10. Decision Record

| Decision | Selected | Rationale |
|----------|----------|-----------|
| Architecture | Option C (Pragmatic) | 6개 파일로 관심사 분리 + 간결함 균형 |
| GUI Framework | ImGui + DX11 | C++ 직접 링크, 게임 업계 표준, 의존성 최소 |
| Chart Library | implot | ImGui 네이티브 통합, 실시간 차트 지원 |
| Threading | GUI + IO 분리 | 60fps 보장 필수, atomic으로 메트릭 전달 |
| Metrics Transfer | atomic + lock-free ring buffer | 락 없이 IO→GUI 데이터 전달 |

---

## 11. Implementation Guide

### 11.1 Implementation Order

| # | Task | Files | Depends On | Est |
|---|------|-------|------------|-----|
| 1 | vcpkg에 imgui, implot 추가 + 빌드 확인 | vcpkg.json | - | 30min |
| 2 | vcxproj 생성 + DX11/ImGui 빈 창 | FastPortTestClient.cpp, .vcxproj | 1 | 2h |
| 3 | TestSession 구현 (RTT 측정) | TestSession.ixx | - | 1h |
| 4 | MetricsCollector 구현 | MetricsCollector.ixx | - | 2h |
| 5 | TestClientApp UI 레이아웃 | TestClientApp.ixx | 2,3,4 | 3h |
| 6 | TestRunner 구현 (연결 + 에코) | TestRunner.ixx | 3,4 | 2h |
| 7 | 차트 통합 (implot) | TestClientApp.ixx | 4,5 | 2h |
| 8 | 스케일 테스트 | TestRunner.ixx | 6 | 1h |
| 9 | ABCompare 구현 | ABCompare.ixx | 6 | 3h |
| 10 | 통합 테스트 + 버그 수정 | All | 1-9 | 3h |

### 11.2 Critical Path

```
1 (vcpkg) → 2 (빈 창) → 5 (UI) → 7 (차트) → 10 (통합)
                           ↑
              3 (Session) ──┤
              4 (Metrics) ──┤
              6 (Runner) ───┘→ 8 (Scale) → 9 (A/B)
```

### 11.3 Session Guide

| Module | Scope Key | Files | Est | Dependencies |
|--------|-----------|-------|-----|-------------|
| module-1: Bootstrap | imgui-setup | vcpkg.json, .vcxproj, FastPortTestClient.cpp | 3h | None |
| module-2: Core | session-metrics | TestSession.ixx, MetricsCollector.ixx | 3h | None |
| module-3: UI | ui-layout | TestClientApp.ixx | 3h | module-1, module-2 |
| module-4: Tests | test-runner | TestRunner.ixx | 3h | module-2 |
| module-5: A/B | ab-compare | ABCompare.ixx | 3h | module-4 |

**Recommended Session Plan:**
- Session 1: module-1 + module-2 (Bootstrap + Core, 병렬 가능)
- Session 2: module-3 + module-4 (UI + Tests)
- Session 3: module-5 + 통합 테스트 (A/B + Polish)

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-04-15 | Initial draft (Option C selected) | AnYounggun |
