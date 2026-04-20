# server-status Design Document

> **Summary**: `ISessionStats` 인터페이스 + `ServerStatsCollector` + `StatsSampler` + `AdminPacketHandler` 의 완전 분리 Clean 구조. Protocols/Admin.proto 로 메시지 정의, FastPortTestClient 에 Admin 탭 신설.
>
> **Project**: FastPort
> **Author**: AnYounggun
> **Date**: 2026-04-20
> **Status**: Draft
> **Planning Doc**: [server-status.plan.md](../../01-plan/features/server-status.plan.md)

---

## Context Anchor

| Key | Value |
|-----|-------|
| **WHY** | Service 모드 서버의 실시간 관찰 수단 필요 (로그만으론 부족) |
| **WHO** | FastPort 개발자/운영자/데모 대상 |
| **RISK** | Admin DoS 노출, SessionList 대용량 직렬화, bytes atomic 성능 |
| **SUCCESS** | Summary ≤ 10ms (1만 세션), 오탐 0, 기존 기능 회귀 0 |
| **SCOPE** | Phase 1: Summary + SessionList(페이징), IOCP+RIO 양쪽, TestClient Admin 탭 |

---

## 1. Overview

### 1.1 Design Goals

1. **타입 불변 Admin 핸들러**: 세션 구체 타입(IOCP/RIO)을 모르게 `ISessionStats` 인터페이스로 접근
2. **관찰 계측의 완전 분리**: Stats Sampler(OS API) / Collector(집계) / PacketHandler(프로토콜) 3 계층
3. **폴링 최적화**: CPU/Memory 는 StatsSampler 가 1Hz 주기 샘플 후 캐시 — 요청마다 OS API 호출 안 함
4. **IOCP/RIO 공통화**: LibNetworks 레이어에 Admin 로직 전부, 각 서버 프로젝트는 Collector 생성 + 핸들러 Dispatch 1줄만
5. **확장성**: 향후 AuthToken, Disconnect API, Prometheus 는 `AdminPacketHandler` 내 추가로 대응 가능

### 1.2 Design Principles

- **Interface Segregation**: `ISessionStats` 는 2 메서드 (`GetTotalRxBytes`, `GetTotalTxBytes`)
- **Dependency Inversion**: `AdminPacketHandler` 와 `ServerStatsCollector` 는 `ISessionStats` 에만 의존
- **Separation of Concerns**: OS API 샘플링(`StatsSampler`) ↔ 집계(`Collector`) ↔ 프로토콜(`Handler`)
- **Cache-Aware**: 폴링 부하 최소화를 위해 CPU/Memory 는 TTL 캐시
- **Snapshot over Iteration**: 세션 목록은 snapshot 후 lock 풀고 직렬화

---

## 2. Architecture Options

### 2.0 Comparison

| Criteria | A: Minimal | **B: Clean (Selected)** | C: Pragmatic |
|----------|:-:|:-:|:-:|
| 인터페이스 | 없음 | **ISessionStats** | 없음 |
| Stats Sampler | 요청마다 실측 | **별도 클래스 + 주기 샘플** | Collector 내부 TTL 캐시 |
| Admin Handler | inline 복제 | **전용 클래스 + DI** | free function |
| 신규 파일 | 1 | **5** | 3 |
| 복잡도 | Low | **High** | Medium |
| 유지보수성 | Low | **Highest** | High |

**Selected: Option B (Clean)** — **Rationale**:
- 사용자 명시적 선택 (일관된 Clean 선호)
- 선행 피처(`session-idle-timeout`)와 동일 패턴(인터페이스 + 분리된 구성요소) → 학습 곡선↓
- StatsSampler 분리로 CPU/Memory 샘플링 로직 단위 테스트 가능
- `ISessionStats` 는 향후 세션별 상세 통계(throughput rate, latency 등) 확장 포인트

### 2.1 Component Diagram

```
┌──────────────────────────────────────────────────────────────────────────┐
│ Protocols/Admin.proto                                                    │
│   AdminStatusSummaryRequest/Response                                     │
│   AdminSessionListRequest/Response (+ AdminSessionInfo)                  │
│   enum ServerMode { UNKNOWN, IOCP, RIO }                                 │
└──────────────────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ Consumer Layer: IOCPServiceMode / RIOServiceMode                         │
│   - ServerStatsCollector 인스턴스 생성/소유                                 │
│   - StatsSampler 인스턴스 생성, Start/Stop                                 │
│   - SnapshotProvider 람다 주입 (SessionContainer → vector<ISessionStats>) │
│   - SessionContainer + Collector ref 를 Inbound 세션에 전달               │
└──────────────────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ networks.admin.admin_packet_handler                                      │
│   class AdminPacketHandler                                               │
│     ctor(ServerStatsCollector& collector)                                │
│     HandlePacket(INetworkSession& sender, const Packet& packet)          │
│       - 0x8001 (SummaryRequest)  → BuildSummary → SendMessage            │
│       - 0x8003 (SessionListReq)  → BuildSessionList(offset, limit) → Send│
└──────────────────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ networks.stats.server_stats_collector                                    │
│   class ServerStatsCollector                                             │
│     ctor(ServerMode, SnapshotProvider, StatsSampler*, IdleCountProvider) │
│     SnapshotSummary()  → {uptimeMs, activeSess, rx, tx, idle, cpu, mem} │
│     SnapshotSessions(offset, limit) → vector<SessionInfo>               │
│                                                                          │
│   depends on:                                                            │
│   - StatsSampler*         (CPU/Memory 캐시값)                             │
│   - SnapshotProvider      () → vector<shared_ptr<ISessionStats>>        │
│   - IdleCountProvider     () → uint64_t (SessionIdleChecker.GetCount)   │
│   - Uptime startTimeMs    (ctor 시점 steady_clock)                        │
└──────────────────────────────────────────────────────────────────────────┘
                          │
                          ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ networks.stats.stats_sampler                                             │
│   class StatsSampler                                                     │
│     ctor(SamplerConfig) + Start / Stop                                   │
│     SnapshotCpuPercent() / SnapshotMemoryBytes() / ServerMode()          │
│                                                                          │
│   - TimerQueue::SchedulePeriodic 으로 1Hz 샘플                             │
│   - GetProcessTimes(prev,curr) delta → CPU %                             │
│   - GetProcessMemoryInfo() → WorkingSetSize                              │
│   - 내부 atomic 캐시 (콜백 write, 요청 read)                                │
└──────────────────────────────────────────────────────────────────────────┘

                          ▲
                          │ implements
                          │
┌──────────────────────────────────────────────────────────────────────────┐
│ networks.sessions.isession_stats                                         │
│   struct ISessionStats                                                   │
│     - GetTotalRxBytes() const noexcept                                   │
│     - GetTotalTxBytes() const noexcept                                   │
│                                                                          │
│ networks.sessions.io_session   (수정)                                     │
│   class IOSession : ..., public ISessionStats                            │
│     - atomic<uint64_t> m_TotalRxBytes, m_TotalTxBytes                    │
│     - OnIOCompleted 수신/송신 경로에서 fetch_add                           │
│                                                                          │
│ networks.sessions.rio_session  (수정)                                     │
│   class RIOSession : ..., public ISessionStats                           │
│     - 동일 atomic 멤버 + RIO 수신/송신 완료 경로에서 갱신                       │
└──────────────────────────────────────────────────────────────────────────┘
```

### 2.2 Data Flow

#### AdminStatusSummaryRequest 처리

```
TestClient Polling (1Hz)
    │ SendMessage(PACKET_ID=0x8001, AdminStatusSummaryRequest)
    ▼
[Network] → IOCP/RIO InboundSession::OnPacketReceived
    │ packetId==0x8001 → m_AdminHandler.HandlePacket(*this, packet)
    ▼
AdminPacketHandler::HandlePacket
    │ 1) packet.ParseMessage(AdminStatusSummaryRequest)
    │ 2) m_Collector.SnapshotSummary()
    │      - StatsSampler.SnapshotCpu/Memory (캐시 read)
    │      - Uptime = NowMs() - m_StartMs
    │      - SnapshotProvider() → vector<ISessionStats>
    │          - 합산: activeCount, totalRx, totalTx
    │      - IdleCountProvider() → idleDisconnectCount
    │ 3) 응답 메시지 조립
    │ 4) sender.SendMessage(0x8002, AdminStatusSummaryResponse)
    ▼
[Network] → TestClient 수신
```

#### AdminSessionListRequest 처리

```
TestClient: 사용자가 "Refresh Sessions" 버튼 클릭
    │ SendMessage(0x8003, AdminSessionListRequest{offset=0, limit=100})
    ▼
AdminPacketHandler::HandlePacket
    │ limit = min(limit, 1000)  // clamp
    │ m_Collector.SnapshotSessions(offset, limit)
    │   - SnapshotProvider() → vector<ISessionStats>
    │   - skip offset, take limit
    │   - 각 세션: id + lastRecvMs + rx + tx 추출
    │ 응답 조립 → sender.SendMessage(0x8004, ...)
    ▼
TestClient: ImGui 테이블에 렌더
```

#### StatsSampler 주기 샘플

```
StatsSampler::Start()
    │ TimerQueue.SchedulePeriodic(1s, [this]{OnTick();}, "StatsSampler")
    ▼
OnTick (TimerQueue 워커)
    │ GetProcessTimes(curr)
    │ cpuPct = (curr - prev) / (wall-delta * numCores) * 100
    │ GetProcessMemoryInfo() → workingSet
    │ m_CpuCache.store(cpuPct, relaxed)
    │ m_MemoryCache.store(workingSet, relaxed)
    │ prev = curr
```

### 2.3 Dependencies

| Component | Depends On | Purpose |
|-----------|-----------|---------|
| `AdminPacketHandler` | `ServerStatsCollector`, Protocols/Admin.pb | 프로토콜 dispatch + 응답 조립 |
| `ServerStatsCollector` | `StatsSampler`, `SnapshotProvider`, `IdleCountProvider` | 집계 |
| `StatsSampler` | `TimerQueue`, Windows API | OS 메트릭 샘플 |
| `IOSession`/`RIOSession` | `ISessionStats` | 바이트 카운터 노출 |
| `IOCPServiceMode`/`RIOServiceMode` | 위 전체 | 생명주기 관리 |
| Consumer Test (FastPortTestClient) | Protocols/Admin.pb | 클라이언트 측 프로토콜 |

---

## 3. Data Model

### 3.1 Protobuf (Protocols/Admin.proto)

```proto
syntax = "proto3";
package fastport.protocols.admin;

import "Commons.proto";

enum ServerMode {
  SERVER_MODE_UNKNOWN = 0;
  SERVER_MODE_IOCP    = 1;
  SERVER_MODE_RIO     = 2;
}

message AdminStatusSummaryRequest {
  fastport.protocols.commons.MessageHeader header = 1;
  string auth_token = 2;  // Phase 1: 서버가 무시
}

message AdminStatusSummaryResponse {
  fastport.protocols.commons.MessageHeader header = 1;
  fastport.protocols.commons.ResultCode    result = 2;
  uint64     server_uptime_ms       = 3;
  uint32     active_session_count   = 4;
  uint64     total_rx_bytes         = 5;
  uint64     total_tx_bytes         = 6;
  uint64     idle_disconnect_count  = 7;
  ServerMode server_mode            = 8;
  uint64     process_memory_bytes   = 9;
  double     process_cpu_percent    = 10;
  uint64     server_timestamp_ms    = 11;
}

message AdminSessionListRequest {
  fastport.protocols.commons.MessageHeader header = 1;
  uint32 offset     = 2;
  uint32 limit      = 3;  // 서버가 1000 으로 clamp
  string auth_token = 4;
}

message AdminSessionInfo {
  uint64 session_id    = 1;
  int64  last_recv_ms  = 2;
  uint64 rx_bytes      = 3;
  uint64 tx_bytes      = 4;
}

message AdminSessionListResponse {
  fastport.protocols.commons.MessageHeader header = 1;
  fastport.protocols.commons.ResultCode    result = 2;
  uint32                                   total  = 3;
  uint32                                   offset = 4;
  repeated AdminSessionInfo                sessions = 5;
}
```

### 3.2 Packet ID 예약

```cpp
// LibNetworks 또는 Protocols 공용 헤더
constexpr uint16_t kPacketId_AdminStatusSummaryRequest  = 0x8001;
constexpr uint16_t kPacketId_AdminStatusSummaryResponse = 0x8002;
constexpr uint16_t kPacketId_AdminSessionListRequest    = 0x8003;
constexpr uint16_t kPacketId_AdminSessionListResponse   = 0x8004;
// 0x8000 ~ 0x8FFF 는 admin 대역 (향후 확장용 예약)
```

### 3.3 ISessionStats Interface

```cpp
// LibNetworks/ISessionStats.ixx
module;
#include <cstdint>
export module networks.sessions.isession_stats;

namespace LibNetworks::Sessions
{
export struct ISessionStats
{
    virtual ~ISessionStats() = default;
    // Thread-safety: atomic read
    virtual std::uint64_t GetTotalRxBytes() const noexcept = 0;
    virtual std::uint64_t GetTotalTxBytes() const noexcept = 0;
};
}
```

### 3.4 IOSession / RIOSession 확장

```cpp
// IOSession.ixx 추가부
class IOSession : ..., public IIdleAware, public ISessionStats, ...
{
public:
    std::uint64_t GetTotalRxBytes() const noexcept override { return m_TotalRxBytes.load(std::memory_order_relaxed); }
    std::uint64_t GetTotalTxBytes() const noexcept override { return m_TotalTxBytes.load(std::memory_order_relaxed); }
protected:
    std::atomic<std::uint64_t> m_TotalRxBytes { 0 };
    std::atomic<std::uint64_t> m_TotalTxBytes { 0 };
};

// IOSession.cpp — OnIOCompleted Real Recv 성공 경로에 추가:
//   m_TotalRxBytes.fetch_add(bytesTransferred, std::memory_order_relaxed);
// IOSession.cpp — Send 완료 성공 경로에 추가:
//   m_TotalTxBytes.fetch_add(bytesTransferred, std::memory_order_relaxed);
```

`RIOSession` 도 동일 패턴 — `OnRioIOCompleted` 의 수신/송신 완료 위치에 fetch_add.

---

## 4. API Specification

### 4.1 StatsSampler

```cpp
// LibNetworks/StatsSampler.ixx
export module networks.stats.stats_sampler;

import std;

namespace LibNetworks::Stats
{
export struct SamplerConfig
{
    std::chrono::milliseconds tickIntervalMs { 1000 };  // 1Hz 기본
    bool                      enabled        { true };
};

export class StatsSampler
{
public:
    explicit StatsSampler(SamplerConfig cfg = {});
    ~StatsSampler();
    StatsSampler(const StatsSampler&) = delete;

    void Start();  // TimerQueue 에 Periodic tick 등록
    void Stop();   // 진행 중 콜백 완료 대기 후 정리

    // 최근 샘플 (캐시) — 요청마다 OS API 호출 안 함.
    double        SnapshotCpuPercent()  const noexcept;  // 0.0 ~ 100.0 * numCores (또는 정규화)
    std::uint64_t SnapshotMemoryBytes() const noexcept;  // WorkingSetSize

    // 호출자가 명시적으로 강제 샘플 (테스트용).
    void ForceSampleNow();

private:
    void OnTick();
    // Windows GetProcessTimes 결과 저장 (이전/현재)
    // atomic double CPU, atomic uint64_t Memory
};
}
```

### 4.2 ServerStatsCollector

```cpp
// LibNetworks/ServerStatsCollector.ixx
export module networks.stats.server_stats_collector;

import std;
import networks.sessions.isession_stats;
import networks.stats.stats_sampler;
// Admin.pb 직접 import 대신, Collector 는 POD struct 반환하고 Handler 가 proto 로 변환

namespace LibNetworks::Stats
{
export enum class ServerMode : std::uint8_t { Unknown = 0, IOCP = 1, RIO = 2 };

export struct SummaryData
{
    std::int64_t  uptimeMs;
    std::uint32_t activeSessionCount;
    std::uint64_t totalRxBytes;
    std::uint64_t totalTxBytes;
    std::uint64_t idleDisconnectCount;
    ServerMode    serverMode;
    std::uint64_t processMemoryBytes;
    double        processCpuPercent;
    std::int64_t  serverTimestampMs;
};

export struct SessionInfoData
{
    std::uint64_t sessionId;
    std::int64_t  lastRecvMs;
    std::uint64_t rxBytes;
    std::uint64_t txBytes;
};

export struct SessionListData
{
    std::uint32_t                total;    // 전체 활성 세션 수
    std::uint32_t                offset;
    std::vector<SessionInfoData> sessions;
};

export class ServerStatsCollector
{
public:
    using SnapshotProvider   = std::function<std::vector<std::shared_ptr<Sessions::ISessionStats>>()>;
    using IdleCountProvider  = std::function<std::uint64_t()>;

    ServerStatsCollector(
        ServerMode serverMode,
        SnapshotProvider sessionProvider,
        IdleCountProvider idleCountProvider,
        StatsSampler* pSampler);   // non-owning, 외부가 수명 관리

    SummaryData     SnapshotSummary() const;
    SessionListData SnapshotSessions(std::uint32_t offset, std::uint32_t limit) const;

    static constexpr std::uint32_t kMaxLimit = 1000;

private:
    ServerMode        m_ServerMode;
    SnapshotProvider  m_SessionProvider;
    IdleCountProvider m_IdleCountProvider;
    StatsSampler*     m_pSampler;  // nullable — 없으면 CPU/Memory = 0
    std::int64_t      m_StartTimeMs;
};
}
```

**설계 포인트**: Collector 는 `AdminStatusSummaryResponse` 같은 protobuf 메시지를 **반환하지 않음**. 순수 POD struct (`SummaryData` 등) 만 반환 → Collector 는 LibNetworks 에서 Protocols 의존 없음. 변환은 `AdminPacketHandler` 가 담당.

### 4.3 AdminPacketHandler

```cpp
// LibNetworks/AdminPacketHandler.ixx
module;
#include <WinSock2.h>
export module networks.admin.admin_packet_handler;

import std;
import networks.sessions.inetwork_session;
import networks.core.packet;
import networks.stats.server_stats_collector;

namespace LibNetworks::Admin
{
// Admin 패킷 ID 상수.
export constexpr std::uint16_t kPacketId_SummaryRequest   = 0x8001;
export constexpr std::uint16_t kPacketId_SummaryResponse  = 0x8002;
export constexpr std::uint16_t kPacketId_SessionListReq   = 0x8003;
export constexpr std::uint16_t kPacketId_SessionListRes   = 0x8004;

export class AdminPacketHandler
{
public:
    explicit AdminPacketHandler(Stats::ServerStatsCollector& collector);

    // 패킷 ID 가 admin 대역(0x8xxx)이면 true 반환 + 응답 송신.
    // 아니면 false 반환 (호출자가 다른 핸들러 dispatch).
    bool HandlePacket(Sessions::INetworkSession& sender,
                      const Core::Packet& packet);

private:
    void HandleSummaryRequest(Sessions::INetworkSession& sender, const Core::Packet& packet);
    void HandleSessionListRequest(Sessions::INetworkSession& sender, const Core::Packet& packet);

    Stats::ServerStatsCollector& m_Collector;
};
}
```

### 4.4 소비자 통합 예 (IOCPInboundSession.cpp)

```cpp
// OnPacketReceived 에 최소 분기 추가:
void IOCPInboundSession::OnPacketReceived(const Packet& rfPacket)
{
    __super::OnPacketReceived(rfPacket);

    const auto packetId = rfPacket.GetPacketId();

    // Admin 패킷은 전역 핸들러에 위임. ServiceMode 가 설정.
    if ((packetId & 0xF000) == 0x8000) {
        if (AdminPacketHandler* pAdmin = GetGlobalAdminHandler()) {
            if (pAdmin->HandlePacket(*this, rfPacket)) return;
        }
    }

    switch (packetId) { /* 기존 ECHO / BENCHMARK */ }
}
```

`GetGlobalAdminHandler()` 는 `IOCPServiceMode` 가 OnStarted 에서 set, OnShutdown 에서 clear. SingleTon 대신 ServiceMode 가 소유한 raw pointer 를 anonymous namespace 의 `std::atomic<AdminPacketHandler*>` 에 저장하는 간단한 방식 (수명은 ServiceMode 에서 관리).

---

## 5. UI/UX (FastPortTestClient Admin 탭)

### 5.1 Layout

```
┌────────────────────────────────────────────────────────────────┐
│ Connection | Echo | Benchmark | Metrics | A/B | [Admin]  [Log] │  ← 탭 바
├────────────────────────────────────────────────────────────────┤
│ ┌──────────────────────────────────────────────────────────┐   │
│ │ Server Summary                    [Poll Enabled ☑ 1 Hz] │   │
│ │ ─────────────────────────────────────────────────────── │   │
│ │ Server Mode       : IOCP                                │   │
│ │ Uptime            : 01:23:45                            │   │
│ │ Active Sessions   : 42                                  │   │
│ │ Total RX          : 12.3 MB                             │   │
│ │ Total TX          : 8.7 MB                              │   │
│ │ Idle Disconnects  : 3                                   │   │
│ │ Process Memory    : 128 MB                              │   │
│ │ Process CPU       : 4.2 %                               │   │
│ │ Server Timestamp  : 2026-04-20 14:32:10                 │   │
│ └──────────────────────────────────────────────────────────┘   │
│                                                                │
│ ┌──────────────────────────────────────────────────────────┐   │
│ │ RX/TX Rate (last 60s)         [implot line graph]       │   │
│ └──────────────────────────────────────────────────────────┘   │
│                                                                │
│ ┌──────────────────────────────────────────────────────────┐   │
│ │ Session List            [Refresh] offset:[0] limit:[100]│   │
│ │ ─────────────────────────────────────────────────────── │   │
│ │ ID       │ LastRecv     │ RX       │ TX                 │   │
│ │ 1        │ 1234ms ago   │ 5.2 KB   │ 2.1 KB             │   │
│ │ 2        │ 456ms ago    │ 128 B    │ 256 B              │   │
│ │ ...                                                      │   │
│ └──────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────┘
```

### 5.2 User Flow

```
1) 사용자 Connection 탭에서 서버 연결
2) Admin 탭 클릭
3) Polling 기본 ON: 1초마다 AdminStatusSummaryRequest 송신 → 화면 갱신
4) RX/TX Rate 그래프: 최근 60개 샘플 rolling, implot 라인
5) [Refresh Sessions] 클릭 시 AdminSessionListRequest → 테이블 업데이트
6) offset/limit 조정 후 Refresh 로 페이징
```

### 5.3 Page UI Checklist

#### Admin Tab
- [ ] Checkbox: Polling Enabled (기본 ON)
- [ ] Display: Server Mode (IOCP/RIO/Unknown 라벨)
- [ ] Display: Uptime (HH:MM:SS 포맷)
- [ ] Display: Active Sessions (숫자)
- [ ] Display: Total RX / TX (자동 단위: B/KB/MB/GB)
- [ ] Display: Idle Disconnect Count (숫자)
- [ ] Display: Process Memory (자동 단위)
- [ ] Display: Process CPU Percent (소수점 1자리)
- [ ] Display: Server Timestamp
- [ ] Graph: RX/TX Rate line chart (implot, 60s rolling)
- [ ] Table: Session List (columns: ID, LastRecv, RX, TX)
- [ ] Inputs: offset (uint), limit (uint, clamp 1-1000)
- [ ] Button: "Refresh Sessions"

---

## 6. Error Handling

### 6.1 Error Scenarios

| # | 상황 | 동작 | Logger |
|---|------|------|--------|
| 1 | 인증 토큰 제공했으나 무시 (Phase 1) | 경고 없이 처리 | (없음) |
| 2 | limit=0 또는 limit > 1000 | 1000 으로 clamp | Debug 로그 |
| 3 | offset > total | 빈 배열 반환, total 필드에 실제 total | (없음) |
| 4 | StatsSampler 미초기화 상태 (Start 전) | CPU/Memory = 0 반환 | Warning 1회만 |
| 5 | SnapshotProvider 예외 | catch, empty vector 로 진행 | Error |
| 6 | IdleCountProvider nullable | 0 으로 기록 | (없음) |
| 7 | proto 파싱 실패 | 응답 `ResultCode::PARSE_ERROR`, 세부 사유 로그 | Error |
| 8 | AdminPacketHandler 미설정 상태에서 admin 패킷 수신 | 경고 + 무시, 연결 유지 | Warning |

### 6.2 Logging Contract

| 시점 | Level | Category | Message |
|---|---|---|---|
| Summary 요청 수신 | Debug | AdminHandler | "Summary request from session {id}" |
| SessionList 요청 수신 | Debug | AdminHandler | "SessionList request from session {id}, offset={}, limit={}" |
| 응답 송신 | Debug | AdminHandler | "Response sent, size={}" |
| 파싱 실패 | Error | AdminHandler | "Parse failed: {}" |
| StatsSampler 시작 | Info | StatsSampler | "Started. TickIntervalMs={}" |
| StatsSampler 중지 | Info | StatsSampler | "Stopped" |
| ServerStatsCollector 첫 Summary 호출 | Debug | ServerStats | "First summary, uptime={}" |

---

## 7. Security Considerations

- **Phase 1 무인증**: 누구든 admin 패킷 보낼 수 있음 → **내부망/개발 전제**
- **DoS 완화**: SessionList limit 1000 강제 clamp. Summary 는 캐시 의존 (폴링 부하 무시 가능)
- **정보 노출**: 세션 id 는 내부 증분 카운터 (IP/인증정보 없음) → 현재 수준 OK
- **향후**: `auth_token` proto 필드 예약됨 → Phase 2 에서 `AdminTokenValidator` 주입 방식으로 확장

---

## 8. Test Plan

### 8.1 Scope

| Type | Target | Tool | Phase |
|---|---|---|---|
| L1 Unit | Admin proto roundtrip | LibNetworksTests | Do |
| L1 Unit | IOSession bytes 카운터 갱신 | LibNetworksTests | Do |
| L1 Unit | ServerStatsCollector 집계 | LibNetworksTests | Do |
| L1 Unit | StatsSampler CPU/Memory 샘플 | LibNetworksTests | Do |
| L1 Unit | AdminPacketHandler dispatch | LibNetworksTests | Do |
| L3 Manual | FastPortServer + TestClient Admin 탭 실시간 값 확인 | 수동 | Do |

### 8.2 L1 — Admin Protocol Tests (`AdminProtocolTests.cpp`)

| # | Test | Assertion |
|---|------|-----------|
| AP-01 | `SummaryRequest_Roundtrip` | 필드 채워 직렬화 → 역직렬화 → 값 일치 |
| AP-02 | `SummaryResponse_AllFields` | 10개 필드 모두 write/read 일치 |
| AP-03 | `SessionListRequest_OffsetLimit` | offset/limit 라운드트립 |
| AP-04 | `SessionListResponse_WithSessions` | sessions 배열 3개 포함 라운드트립 |
| AP-05 | `ServerMode_Enum_IOCP_RIO_Unknown` | enum 값 3종 모두 보존 |

### 8.3 L1 — IOSession Bytes Tests

| # | Test | Assertion |
|---|------|-----------|
| IB-01 | `IOSession_GetTotalRx_InitialZero` | 생성 직후 0 |
| IB-02 | `IOSession_RxBytes_UpdatedOnReceive` | Mock recv 1024 bytes → 카운터 1024 |
| IB-03 | `IOSession_TxBytes_UpdatedOnSend` | Mock send 512 → 카운터 512 |
| IB-04 | `IOSession_ZeroByte_NoUpdate` | zero-byte Recv 완료는 rx 갱신 안 함 |
| IB-05 | `IOSession_Cumulative_AcrossMultipleOps` | 3번 수신(100+200+300) → 합 600 |

### 8.4 L1 — ServerStatsCollector Tests

Mock `ISessionStats`:
```cpp
struct MockSessionStats : public ISessionStats {
    std::uint64_t rx, tx;
    std::uint64_t GetTotalRxBytes() const noexcept override { return rx; }
    std::uint64_t GetTotalTxBytes() const noexcept override { return tx; }
};
```

| # | Test | Assertion |
|---|------|-----------|
| SC-01 | `Summary_EmptyProvider` | activeSessionCount=0, totalRx=0, totalTx=0 |
| SC-02 | `Summary_ThreeSessions_Aggregated` | 3 Mock (rx 100/200/300) → totalRx=600 |
| SC-03 | `Summary_UptimeIncreasesOverTime` | ctor 후 sleep 100ms → uptime ≥ 100 |
| SC-04 | `Summary_IdleCountFromProvider` | IdleCountProvider=() → 42 → idle=42 |
| SC-05 | `Summary_NoSampler_CpuMemoryZero` | Sampler=nullptr → cpu=0, memory=0 |
| SC-06 | `SessionList_OffsetLimit_Paging` | 5 sessions, offset=1 limit=2 → 2 entries, total=5 |
| SC-07 | `SessionList_LimitClamp` | limit=5000 → 1000 으로 clamp |
| SC-08 | `SessionList_OffsetOverflow_EmptyResult` | offset=100 / total=5 → empty array, total=5 |

### 8.5 L1 — StatsSampler Tests

| # | Test | Assertion |
|---|------|-----------|
| SS-01 | `Sampler_InitialSample_ZeroCpu` | 첫 호출 CPU=0 (prev 없음) |
| SS-02 | `Sampler_AfterStart_MemoryPositive` | Start + ForceSampleNow → memory > 0 |
| SS-03 | `Sampler_Stop_NoFurtherSample` | Stop 후 ForceSampleNow 는 허용되나 tick 은 멈춤 |
| SS-04 | `Sampler_Disabled_NeverTicks` | enabled=false → Start 후 샘플 없음 |

### 8.6 L1 — AdminPacketHandler Tests

Fake session for capturing SendMessage:
```cpp
struct FakeSession : public INetworkSession {
    std::vector<std::pair<uint16_t, std::string>> sentMessages;
    void SendMessage(uint16_t id, const google::protobuf::Message& msg) override {
        sentMessages.emplace_back(id, msg.SerializeAsString());
    }
    // ... 기타 인터페이스 기본 구현
};
```

| # | Test | Assertion |
|---|------|-----------|
| AH-01 | `Handle_NonAdminPacket_ReturnsFalse` | packet id=0x1001 (Echo) → HandlePacket returns false |
| AH-02 | `Handle_SummaryRequest_SendsResponse` | 0x8001 요청 → FakeSession.sent 에 0x8002 응답 1개 |
| AH-03 | `Handle_SummaryResponse_FieldsPopulated` | parse 한 response 의 active_session_count 등 검증 |
| AH-04 | `Handle_SessionListRequest_Pagination` | offset=2 limit=3 → response.sessions.size()==3 |
| AH-05 | `Handle_MalformedPacket_LogsError` | 깨진 바이트 → HandlePacket returns true (consumed), error 로그 |

### 8.7 L3 — Manual Scenario

- FastPortServer 실행 → FastPortTestClient 연결 → Admin 탭
- 1초마다 Summary 값 갱신 확인
- Echo 테스트 반복 실행 → total_rx/tx 증가 관찰
- 여러 클라이언트 동시 접속 → active_session_count 변화 확인
- TestClient 하나 강제 종료 → 11초 후 idle_disconnect_count 증가 확인
- SessionList offset/limit 조작 → 페이징 동작 확인
- FastPortServerRIO 도 동일 검증

---

## 9. Clean Architecture

### 9.1 Layer Structure

| Layer | Location | Responsibility |
|---|---|---|
| **Protocol** | `Protocols/Admin.proto` | 메시지 정의, 언어 독립 |
| **Interface** | `LibNetworks/ISessionStats.ixx` | 세션 통계 추상화 |
| **Domain** | `LibNetworks/ServerStatsCollector.{ixx,cpp}` | 비즈니스 집계 로직 (protobuf 무관) |
| **Infrastructure** | `LibNetworks/StatsSampler.{ixx,cpp}` | OS API (Windows) |
| **Application** | `LibNetworks/AdminPacketHandler.{ixx,cpp}` | 프로토콜 ↔ Collector 어댑터 |
| **Existing Abstraction** | `IOSession`/`RIOSession` (수정) | `ISessionStats` 구현 |
| **Consumer** | `IOCPServiceMode`/`RIOServiceMode` (수정) | DI + 생명주기 |
| **Client UI** | `FastPortTestClient` Admin 탭 | 조회·렌더링 |

### 9.2 Module Dependencies

```
Consumer (IOCPServiceMode/RIOServiceMode)
  │
  ├──> networks.admin.admin_packet_handler
  │        │
  │        └──> networks.stats.server_stats_collector
  │                 │
  │                 ├──> networks.stats.stats_sampler
  │                 │        └──> commons.timer_queue (주기 샘플)
  │                 │
  │                 └──> networks.sessions.isession_stats
  │                          ▲
  │                          │ implements
  │                          │
  │              networks.sessions.io_session (수정)
  │              networks.sessions.rio_session (수정)
  │
  └──> Protocols/Admin.pb (proto 메시지)
```

---

## 10. Coding Convention Reference

- Module 네이밍: `networks.admin.admin_packet_handler`, `networks.stats.server_stats_collector`, `networks.stats.stats_sampler`, `networks.sessions.isession_stats`
- 로그 카테고리: `"AdminHandler"`, `"ServerStats"`, `"StatsSampler"`
- 로깅 헬퍼: 선행 피처와 동일 `std::format` + 단일 `const std::string&` 시그니처 (ICE 회피)
- GMF 에 `#include <spdlog/spdlog.h>` 필수
- Time: `std::chrono::steady_clock`, `NowMs()` 헬퍼 재사용
- 예외 정책: public API throw 금지, OnTick/HandlePacket catch-all

---

## 11. Implementation Guide

### 11.1 File Structure

```
Protocols/
└── Admin.proto                       (신규)

LibNetworks/
├── ISessionStats.ixx                 (신규)
├── IOSession.ixx, IOSession.cpp      (수정: ISessionStats 구현 + bytes 카운터)
├── RIOSession.ixx, RIOSession.cpp    (수정: 동일)
├── StatsSampler.ixx, StatsSampler.cpp       (신규)
├── ServerStatsCollector.ixx, *.cpp   (신규)
└── AdminPacketHandler.ixx, *.cpp     (신규)

FastPortServer/
├── IOCPInboundSession.cpp            (수정: OnPacketReceived admin 분기)
└── IOCPServiceMode.{ixx,cpp}         (수정: Sampler+Collector+Handler 생명주기)

FastPortServerRIO/
├── RIOInboundSession.cpp             (수정: 동일)
└── RIOServiceMode.{ixx,cpp}          (수정: 동일)

FastPortTestClient/
├── (신규) AdminTab.ixx, AdminTab.cpp  또는 기존 TestClientApp 내에 통합
└── TestClientApp.ixx/cpp             (수정: Admin 탭 추가)

LibNetworksTests/
├── AdminProtocolTests.cpp            (신규)
├── ServerStatsCollectorTests.cpp     (신규)
├── StatsSamplerTests.cpp             (신규)
└── AdminPacketHandlerTests.cpp       (신규)
```

### 11.2 Implementation Order

1. [ ] **M1 — Protocols/Admin.proto**: 메시지 정의 + vcxproj 등록, 빌드 확인
2. [ ] **M2 — ISessionStats 인터페이스**: 신규 모듈
3. [ ] **M3 — IOSession/RIOSession 바이트 카운터**: atomic 추가 + OnIOCompleted 갱신
4. [ ] **M4 — StatsSampler**: TimerQueue 사용, CPU/Memory 캐시 + 단위 테스트
5. [ ] **M5 — ServerStatsCollector**: SnapshotSummary/SnapshotSessions + 단위 테스트
6. [ ] **M6 — AdminPacketHandler**: HandlePacket, SummaryRequest/SessionListRequest 처리 + 테스트
7. [ ] **M7 — IOCPServiceMode 연동**: Sampler/Collector/Handler 생성·종료 + InboundSession 에서 dispatch
8. [ ] **M8 — FastPortServerRIO 연동**: 동일
9. [ ] **M9 — FastPortTestClient Admin 탭**: UI + 폴링 + 테이블
10. [ ] **M10 — 수동 시나리오 검증**: 값 업데이트/페이징/모드별 동작 확인

### 11.3 Session Guide

#### Module Map

| Module | Scope Key | Description | Est. Turns |
|---|---|---|:-:|
| Protocols + Interface | `proto,stats` | Admin.proto, ISessionStats, IOSession/RIOSession 바이트 카운터 | 10-14 |
| Sampler + Collector | `collector` | StatsSampler, ServerStatsCollector + 단위 테스트 | 18-22 |
| Admin Handler | `handler` | AdminPacketHandler + 단위 테스트 | 12-16 |
| Server 연동 | `integration` | IOCPServiceMode/RIOServiceMode + Inbound 세션 dispatch | 10-14 |
| TestClient UI | `testclient-ui` | Admin 탭 + 폴링 + 테이블·그래프 | 20-25 |

#### Recommended Session Plan

| Session | Phase | Scope | Turns |
|---|---|---|:-:|
| Session 1 (완료) | Plan + Design | 전체 | ~25 |
| Session 2 | Do | `--scope proto,stats` | 12-18 |
| Session 3 | Do | `--scope collector` | 18-22 |
| Session 4 | Do | `--scope handler` | 12-16 |
| Session 5 | Do | `--scope integration` | 12-16 |
| Session 6 | Do | `--scope testclient-ui` | 20-25 |
| Session 7 | Check + Report | 전체 | 15-20 |

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-04-20 | Initial draft (Option B Clean — ISessionStats + Collector + Sampler + Handler 분리) | AnYounggun |
