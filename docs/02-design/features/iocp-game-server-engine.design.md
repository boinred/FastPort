# iocp-game-server-engine Design Document

> **Summary**: IOCP 전용 게임 서버 엔진 v1 의 Clean Architecture 기반 모듈 설계. Recv 경로를 `RecvBufferPool` / `PacketFramerCore` + `FramingPolicy` / `Buffer` 값 타입으로 분리하고, `KeepAliveManager`(App+TCP), `ServerLifecycle`(고정 순서 shutdown), 그리고 기존 `iosession-lifetime-race` 결과를 흡수해 P99 ≤ 80µs / 1M×2 zero crash 목표를 달성한다.
>
> **Project**: FastPort
> **Version**: v1
> **Author**: An Younggun
> **Date**: 2026-04-22
> **Status**: Draft
> **Planning Doc**: [iocp-game-server-engine.plan.md](../../01-plan/features/iocp-game-server-engine.plan.md)
> **Architecture Option**: **Option B — Clean Architecture** (user selected)

---

## Context Anchor

> Copied from Plan document. Strategic context for Design→Do handoff.

| Key | Value |
|-----|-------|
| **WHY** | IOCP 정석 오픈 레퍼런스 부재 + FastPort IOCP 라인 완성도 확보 (RIO 양다리 제거) |
| **WHO** | 한국 C++ 1~5인 인디/미드코어 서버팀 (CCU 1K~20K). Windows, MSVC, C++20, IOCP 경험 |
| **RISK** | ① `std::span` 수명 오해 ② lifetime race edge-case 재발 ③ 벤치 재현성 |
| **SUCCESS** | Peak ≥ 30K PPS@64B / P50 ≤ 30µs / P99 ≤ 80µs / stress 1M×2 zero crash / Echo 샘플 10분 first-run / 로컬+클라우드 동일 벤치 |
| **SCOPE** | M1 race 수정 흡수, M2 Zero-Copy Recv + Per-Thread Pool, M3 KeepAlive(App+TCP), M4 Graceful Shutdown, M5 Echo/Chat + 벤치, M6 한국어 가이드 v2. RIO freeze |

---

## 1. Overview

### 1.1 Design Goals

1. **P99 80µs / 30K+ PPS 유지** — Recv 경로에서 per-packet heap alloc 과 atomic inc/dec 를 제로로 만든다.
2. **lifetime race 재발 차단** — IOSession / Timer / Logger 수명 관계를 명시적 state machine 으로 잠근다.
3. **유닛 테스트 가능성** — Pool / KeepAlive / Lifecycle / FramingPolicy 각각을 `LibNetworks` 로부터 독립 테스트.
4. **RIO 복귀 호환성** — 같은 `RecvBufferPool`, `KeepAliveManager`, `ServerLifecycle`, `Buffer` 를 RIO 축이 나중에 다시 쓸 수 있게 IOCP 전용 의존성을 배제.
5. **한국어 레퍼런스** — 구조 자체가 다이어그램 4장(Recv flow, Pool lifetime, KeepAlive, Shutdown) 으로 설명 가능해야 한다.

### 1.2 Design Principles

- **단일 책임**: 모듈 1개 = 개념 1개. `RecvBufferPool` 은 풀만, `FramingPolicy` 는 프레이밍만.
- **값 타입 우선**: `Buffer` 는 이동 전용 값 타입. 공유가 필요한 경우에만 명시적 변환(`ToShared`).
- **정책 ≠ 메커니즘**: `PacketFramerCore` (메커니즘, span 콜백 dispatch) 와 `FramingPolicy` (정책, length-prefix 등) 분리.
- **고정 순서 Shutdown**: `ServerLifecycle::Shutdown()` 이 유일한 종료 진입점. 순서 역전 금지 규약은 유닛 테스트로 잠근다.
- **API 경계 최소 노출**: 사용자(게임 서버 개발자) 가 인지해야 할 공개 API 는 6개 이하로 제한.

---

## 2. Architecture Options (v1.7.0)

### 2.0 Architecture Comparison

| Criteria | Option A: Minimal | Option B: Clean | Option C: Pragmatic |
|----------|:-:|:-:|:-:|
| **Approach** | IOSession 에 직접 추가 | 모듈 완전 분리 | 핵심 3개만 분리 |
| **New Modules** | 0 | **4~5** | 3 |
| **Modified Files** | ~8 | **~15** | ~10 |
| **Complexity** | Low | High | Medium |
| **Maintainability** | Medium | **High** | High |
| **Effort** | Low | **High** | Medium |
| **Risk** | Coupled 누적 | Low (clean) | Low (balanced) |
| **Recommendation** | 빠른 핫픽스 | **v1 완성/장기 유지** | 기본값 |

**Selected**: **Option B — Clean Architecture**

**Rationale**: 사용자 선택. v1 "IOCP 정석 엔진 완성" 목표에서 (a) `LibNetworksTests` 로 Pool/KeepAlive/Lifecycle 을 독립 검증할 필요가 높고, (b) RIO freeze 해제 시 동일 모듈을 재사용해야 하며, (c) PacketFramer 의 framing policy 는 게임 프로토콜마다 다르므로 정책 확장점이 필수다. 추가 모듈 수 증가 비용을 장기 유지보수성 이득이 초과한다.

### 2.1 Component Diagram

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                             LibNetworks (IOCP v1)                             │
│                                                                              │
│  ┌──────────────────┐   ┌─────────────────────┐   ┌─────────────────────┐    │
│  │  ServerLifecycle │──▶│   IOCP / Acceptor   │──▶│   IOSession (race   │    │
│  │  (state machine) │   │   (worker pool)     │   │   fixed, Option C)  │    │
│  └──────────────────┘   └─────────────────────┘   └─────────┬───────────┘    │
│          │                        │                          │               │
│          │                        ▼                          ▼               │
│          │              ┌─────────────────────┐   ┌─────────────────────┐    │
│          │              │   RecvBufferPool    │   │  KeepAliveManager   │    │
│          │              │   (per-worker TLS + │   │  (App ping/pong +   │    │
│          │              │   crossing queue)   │   │  TCP keepalive)     │    │
│          │              └──────────┬──────────┘   └──────────┬──────────┘    │
│          │                         │                          │              │
│          │                         ▼                          │              │
│          │              ┌─────────────────────┐              │              │
│          │              │  PacketFramerCore   │              │              │
│          │              │  (span dispatch) +  │              │              │
│          │              │  FramingPolicy      │              │              │
│          │              └──────────┬──────────┘              │              │
│          │                         │                          │              │
│          │                         ▼                          │              │
│          │              ┌─────────────────────┐              │              │
│          │              │  OnPacket callback  │              │              │
│          │              │  (user, std::span)  │              │              │
│          │              └─────────────────────┘              │              │
│          │                                                    │              │
│          ▼                                                    │              │
│  ┌──────────────────┐  ┌──────────────────┐  ┌──────────────┘              │
│  │ LibCommons::     │  │  LibCommons::    │                                 │
│  │  TimerQueue      │  │    Logger        │                                 │
│  └──────────────────┘  └──────────────────┘                                 │
└──────────────────────────────────────────────────────────────────────────────┘

Shutdown Order (enforced by ServerLifecycle):
  1. Stop accept  →  2. Drain in-flight IO (timeout)  →  3. Join workers
  4. TimerQueue.Shutdown(true)  →  5. Logger.Shutdown()
```

### 2.2 Data Flow — Recv (happy path)

```
WSARecv completion
      │
      ▼
IOCP worker dequeues OVERLAPPED
      │
      ▼
IOSession::OnRecvComplete(bytes)                    ← [lifetime-race fix active]
      │
      ▼
RecvBufferPool::worker()->AppendRecv(bytes)         ← per-worker TLS pool
      │
      ▼  (assemble stream)
PacketFramerCore::Feed(span<const byte>)
      │
      ▼
FramingPolicy::TryExtract(span) → vector<span packet>
      │
      ▼
for each packet:
   UserCallback(SessionId, std::span<const std::byte>)   ← ZERO-COPY, sync
      │  (user may call Buffer::CopyToHeap() / ToShared() for async)
      ▼
IOSession::PostRecv()                              ← reuse TLS pool slot
```

### 2.3 Dependencies

| Component | Depends On | Purpose |
|-----------|-----------|---------|
| `ServerLifecycle` | `LibCommons::Logger`, `LibCommons::TimerQueue` | 순서 잠금된 shutdown |
| `IOSession` | `RecvBufferPool`, `KeepAliveManager`, `PacketFramerCore`, `LibCommons::Logger` | Recv/KeepAlive/Dispatch |
| `RecvBufferPool` | `LibCommons::Logger`, OS TLS | Per-worker buffer slab |
| `KeepAliveManager` | `LibCommons::TimerQueue`, `LibCommons::Logger`, Winsock `SIO_KEEPALIVE_VALS` | ping/pong + TCP keepalive |
| `PacketFramerCore` | `FramingPolicy` (concept) | Stream → packet 분할 |
| `FramingPolicy` (concept) | nothing | 게임별 확장점 |
| `Buffer` | nothing | 값 타입, helper 함수 |
| `samples/*` | 위 전부 | 사용 예시 |
| `benchmark/*` | 위 전부 | 재현 가능 벤치 |

---

## 3. Data Model

> C++ 네이티브 모듈. DB/REST 없음. "데이터 모델" 은 모듈 간 전달되는 값 타입을 의미.

### 3.1 핵심 타입

```cpp
// commons.buffer (module)
namespace FastPort {

struct Buffer {
  std::byte* data = nullptr;
  std::size_t size = 0;
  std::size_t capacity = 0;
  // 소유권: 이동 전용
  Buffer() = default;
  Buffer(Buffer&&) noexcept;
  Buffer& operator=(Buffer&&) noexcept;
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  ~Buffer();

  std::span<const std::byte> View() const noexcept {
    return { data, size };
  }
};

// 비동기 보관이 필요한 경우의 명시적 변환 (CLAUDE.md 수명 규칙 명시화)
namespace BufferOps {
  Buffer CopyToHeap(std::span<const std::byte> src);
  std::shared_ptr<const Buffer> ToShared(std::span<const std::byte> src);
}

} // namespace FastPort
```

```cpp
// networks.recv_buffer_pool (module)
namespace FastPort::Networks {

struct RecvSlot {
  std::byte storage[kRecvSlotSize];          // kRecvSlotSize = 8192 (tunable)
  std::size_t writePos = 0;
  std::uint32_t ownerWorkerId = 0;           // crossing return 판별용
};

class RecvBufferPool {
public:
  static RecvBufferPool& Instance();
  RecvSlot* Acquire();                        // per-worker TLS 에서 LIFO pop
  void Release(RecvSlot* slot) noexcept;      // owner worker 와 같으면 TLS 로, 다르면 crossing queue 로 defer
  // per-worker crossing queue 를 모든 worker 가 poll (IOCP completion 후 1회 drain)
  void DrainCrossing() noexcept;

  // 관측용
  struct Stats { std::uint64_t acquire, release, crossing, miss; };
  Stats Snapshot() const noexcept;
private:
  // thread_local storage for per-worker free-list
  // + lock-free MPSC crossing queue per worker
};

} // namespace FastPort::Networks
```

```cpp
// networks.framing_policy (module, concept + default impl)
namespace FastPort::Networks {

// 정책 콘셉트 (C++20 concept)
template <typename P>
concept FramingPolicy = requires(P& p, std::span<const std::byte> s,
                                 std::vector<std::span<const std::byte>>& out) {
  { p.TryExtract(s, out) } -> std::convertible_to<std::size_t>; // consumed bytes
};

// 기본 구현: 2-byte little-endian length prefix
class LengthPrefixPolicy {
public:
  std::size_t TryExtract(std::span<const std::byte> stream,
                         std::vector<std::span<const std::byte>>& out) noexcept;
  static constexpr std::size_t kMaxPacketSize = 64 * 1024;  // 64KB hard cap
};

} // namespace FastPort::Networks
```

```cpp
// networks.packet_framer_core (module)
namespace FastPort::Networks {

template <FramingPolicy P = LengthPrefixPolicy>
class PacketFramerCore {
public:
  using OnPacketFn =
    std::function<void(SessionId, std::span<const std::byte>)>;

  explicit PacketFramerCore(OnPacketFn cb);
  void Feed(SessionId sid, std::span<const std::byte> chunk);
  void Reset(SessionId sid);
private:
  P policy_;
  OnPacketFn onPacket_;
  // per-session carry-over stream (완전한 패킷 직전까지 남은 바이트)
  ...
};

} // namespace FastPort::Networks
```

```cpp
// networks.keepalive (module)
namespace FastPort::Networks {

struct KeepAliveConfig {
  // App-level
  bool appPingEnabled = true;
  std::chrono::milliseconds idleBeforePing{5000};
  std::chrono::milliseconds pongTimeout{1000};
  int maxMissedPongs = 3;

  // TCP-level
  bool tcpKeepAliveEnabled = true;
  std::chrono::milliseconds tcpIdle{30000};
  std::chrono::milliseconds tcpInterval{5000};
};

class KeepAliveManager {
public:
  explicit KeepAliveManager(const KeepAliveConfig& cfg);
  void OnSessionOpened(IOSession& s);
  void OnSessionClosed(IOSession& s);
  void OnBytesReceived(IOSession& s);   // idle 타이머 reset
  void OnPongReceived(IOSession& s);

  // 관측용
  struct Stats { std::uint64_t pingsSent, pongsReceived, timeouts; };
  Stats Snapshot() const noexcept;
};

} // namespace FastPort::Networks
```

```cpp
// networks.server_lifecycle (module)
namespace FastPort::Networks {

enum class ServerState : std::uint8_t {
  Created, Running, Draining, Stopping, Stopped
};

class ServerLifecycle {
public:
  void Start(const ServerConfig& cfg);
  // 순서 잠금: accept stop → drain → join → TimerQueue.Shutdown → Logger.Shutdown
  void Shutdown(std::chrono::milliseconds drainTimeout = std::chrono::seconds{10});
  ServerState State() const noexcept;
};

} // namespace FastPort::Networks
```

### 3.2 Entity Relationships

```
ServerLifecycle (1) ──owns──▶ IOCPWorkerPool (1) ──runs──▶ IOSession (N)
         │                                                    │
         ├──owns──▶ Acceptor (1)                              ├──uses──▶ RecvBufferPool (1, TLS)
         │                                                    ├──uses──▶ KeepAliveManager (1)
         └──owns──▶ TimerQueue (shared via LibCommons)         └──uses──▶ PacketFramerCore (1 per session)
                                                                         │
                                                                         └──uses──▶ FramingPolicy (template arg)
```

### 3.3 Storage

N/A — 네이티브 인메모리만. 로그는 `LibCommons::Logger` 파일 싱크.

---

## 4. API Specification

> REST 없음. **공개 C++ API** 목록. 사용자(게임 서버 개발자) 가 반드시 알아야 할 것만.

### 4.1 Endpoint List (공개 API 요약)

| # | API | 위치 | 역할 |
|---|-----|------|-----|
| 1 | `ServerLifecycle::Start(ServerConfig)` | `networks.server_lifecycle` | 엔진 시작 |
| 2 | `ServerLifecycle::Shutdown(timeout)` | `networks.server_lifecycle` | **유일한 종료 진입점** |
| 3 | `OnPacket callback(SessionId, std::span<const std::byte>)` | 사용자 제공 | 패킷 수신 (zero-copy, sync-only) |
| 4 | `BufferOps::CopyToHeap(span)` / `BufferOps::ToShared(span)` | `commons.buffer` | 비동기 보관용 명시 복사 |
| 5 | `KeepAliveConfig` (struct) | `networks.keepalive` | App+TCP keepalive 구성 |
| 6 | `FramingPolicy` concept (+ `LengthPrefixPolicy` default) | `networks.framing_policy` | 프로토콜 확장점 |
| 7 | `ServerStatus` (기존 Admin) | `networks.admin` | Pool/KeepAlive stats 확장 |

### 4.2 상세

#### API #3 — OnPacket callback

```cpp
// 시그니처 (유일한 수신 진입점)
void OnPacket(SessionId sid, std::span<const std::byte> payload);

// 제약사항 (CLAUDE.md 수명 규칙 명시):
//  - payload 의 수명 == 콜백 실행 시간
//  - 비동기로 넘기려면 BufferOps::CopyToHeap(payload) 또는 ToShared(payload) 사용
//  - payload 저장/락에 보관 시 즉시 크래시 대상 (Debug 빌드: magic cookie 검증)
```

**에러 케이스:**

| 상황 | 처리 | 사용자에게 전달되는가? |
|---|---|---|
| FramingPolicy 가 음수/과대 length 반환 (> 64KB) | 해당 세션 즉시 close + `LogError` | ❌ (세션 close 로 암묵 통지) |
| 사용자 콜백에서 예외 throw | catch 후 세션 close + `LogError` | ❌ |
| payload 수명 규칙 위반 (Debug 빌드) | ASSERT + breakpoint | ✅ (디버그) |

#### API #2 — ServerLifecycle::Shutdown

```cpp
void Shutdown(std::chrono::milliseconds drainTimeout = 10s);

// 내부 순서 (유닛 테스트로 잠금):
//   1. state = Draining
//   2. Acceptor.StopAcceptingNewConnections()
//   3. IOSessionRegistry.CloseAll(flushSend = true)
//   4. Wait for in-flight IO completion up to drainTimeout
//   5. IOCPWorkerPool.SignalStop() + Join()
//   6. TimerQueue::GetInstance().Shutdown(true)   ← 반드시 Logger 보다 먼저
//   7. Logger::GetInstance().Shutdown()           ← 최후
//   8. state = Stopped
```

---

## 5. UI/UX Design

> 네이티브 서버 엔진 — GUI 없음. 본 섹션은 **샘플 프로그램 CLI UX** 만 다룸.

### 5.1 Echo 샘플 CLI

```
fastport-echo-server.exe [--port 9000] [--workers N]
fastport-echo-client.exe [--host 127.0.0.1] [--port 9000] [--count 10000]
```

### 5.2 User Flow — Echo sample first-run

```
git clone → cmake/vcpkg restore (auto) → build (x64/Debug) →
  start echo-server → start echo-client → 1 round-trip 성공 ≤ 10분
```

### 5.3 Component List

| Component | Location | 역할 |
|---|---|---|
| `samples/echo/server/main.cpp` | `samples/echo/server/` | 최소 echo 서버 (약 80 LOC) |
| `samples/echo/client/main.cpp` | `samples/echo/client/` | 최소 echo 클라이언트 |
| `samples/chat/*` | `samples/chat/` | 다중 세션 broadcast |
| `samples/echo/README.md` | — | Prerequisites / Build / Run / Expected output / Troubleshooting |
| `benchmark/bench_echo.ps1` | `benchmark/` | Windows 로컬 벤치 |
| `benchmark/bench_echo_cloud.ps1` | `benchmark/` | 클라우드 VM 원격 실행 wrapper |
| `benchmark/ENV_TEMPLATE.md` | `benchmark/` | 환경 블록 템플릿 (결과 markdown 의 첫 섹션) |

### 5.4 Sample README Checklist

> 샘플 README 가 반드시 포함해야 할 항목. 신규 사용자 10분 first-run 기준.

#### `samples/echo/README.md`

- [ ] Prerequisites: Windows 10 1809+, VS 2022, vcpkg, C++20
- [ ] Build steps: `msbuild FastPort.slnx /p:Configuration=Release /p:Platform=x64`
- [ ] Run server: `fastport-echo-server.exe --port 9000`
- [ ] Run client: `fastport-echo-client.exe --host 127.0.0.1 --port 9000 --count 1`
- [ ] Expected output: "[server] echo 5 bytes" / "[client] recv OK"
- [ ] Troubleshooting: port in use, Windows Defender prompt, vcpkg 실패

---

## 6. Error Handling

### 6.1 Error Categories (C++ 네이티브)

| Code | Source | Message | 처리 |
|---|---|---|---|
| E_WIN32_SOCKET | WSAStartup/socket/bind/listen | `::GetLastError()` → Logger + 종료 |
| E_WIN32_IOCP | CreateIoCompletionPort / GetQueuedCompletionStatus | Logger + 해당 worker 재시작 시도, 3회 실패시 Shutdown |
| E_FRAMING_OVERRUN | FramingPolicy | length > 64KB → 세션 close |
| E_USER_EXCEPTION | OnPacket callback throw | catch → 세션 close + Logger |
| E_POOL_CROSSING_OVERFLOW | RecvBufferPool crossing queue full | fallback to heap alloc + Logger warning + counter++ |
| E_KEEPALIVE_TIMEOUT | KeepAliveManager | 정상 동작 — 세션 close + Logger info |
| E_SHUTDOWN_TIMEOUT | ServerLifecycle drain | Logger warning + force join |

### 6.2 Error Logging 포맷

```
// Logger 카테고리는 모듈별 constexpr const char* 로 고정 (CLAUDE.md 컨벤션)
// 예:
constexpr const char* kLogCategoryPool = "RecvBufferPool";
constexpr const char* kLogCategoryKA   = "KeepAliveManager";
constexpr const char* kLogCategoryLife = "ServerLifecycle";
constexpr const char* kLogCategoryFra  = "PacketFramer";

// 호출 패턴:
Logger::GetInstance().LogError(
    kLogCategoryLife,
    std::format("Shutdown drain timeout after {}ms, forcing worker join",
                drainTimeout.count()));
```

---

## 7. Security Considerations

- [ ] **Packet size cap**: FramingPolicy 에서 max 64KB 강제 (DoS 완화)
- [ ] **Session cap**: `ServerConfig::maxSessions` (기본 50000) — 초과 accept 거부
- [ ] **Rate limit (per-session)**: `KeepAliveConfig` 에는 포함하지 않음 — v1.1 에서 별도 모듈
- [ ] **TLS**: **OOS (v1.1+)**. 본 v1 은 평문 TCP 만.
- [ ] **로그 민감 데이터 마스킹**: payload 16바이트만 hex dump (옵션)
- [ ] **Windows Defender / SmartScreen**: 샘플 README 에 예상 경고 + 로컬 signing 가이드

---

## 8. Test Plan (v2.3.0)

### 8.1 Test Scope

| Type | Target | Tool | Phase |
|------|--------|------|-------|
| L1: 유닛 (Module) | `RecvBufferPool`, `LengthPrefixPolicy`, `PacketFramerCore`, `KeepAliveManager`, `ServerLifecycle`, `Buffer` | MSVC Test Framework (`LibNetworksTests`, `LibCommonsTests`) | Do |
| L2: 통합 (in-proc) | Server + Client 동일 프로세스, 실제 Winsock | MSVC Test + `FastPortTestClient` | Do |
| L3: 샘플 E2E | `samples/echo` 전체 빌드/실행 | PowerShell script + assert | Do |
| L4: 벤치/성능 | 64B PPS / 응답 지연 histogram | `FastPortBenchmark` + `benchmark/bench_echo.ps1` | Do / Check |
| L5: 스트레스/수명 | lifetime race reproducer 1M×2 라운드 | `LibNetworksTests` stress | Do / Check |

### 8.2 L1 — Unit Test Scenarios

| # | Module | 시나리오 | 기대 |
|---|---|---|---|
| 1 | RecvBufferPool | Acquire→Release 같은 worker, 반복 N=1M | alloc counter = 1 slot, release counter = 1M |
| 2 | RecvBufferPool | Worker A Acquire → Worker B Release | crossing counter +1, 이후 A 의 TLS 로 복귀 |
| 3 | RecvBufferPool | 동시 8 worker × 100K release | no race, no leak (counter 일치) |
| 4 | LengthPrefixPolicy | 단일 완전 패킷 | consumed = total, out.size = 1 |
| 5 | LengthPrefixPolicy | 반 잘린 패킷 + 다음 chunk | 2 chunk 합쳐 1 패킷 |
| 6 | LengthPrefixPolicy | length > 64KB | 예외/에러 코드, out 빈 상태 |
| 7 | PacketFramerCore | 여러 세션 carry-over 독립성 | session A 데이터가 B 의 out 에 섞이지 않음 |
| 8 | KeepAliveManager | idle 5s → ping 발사 | timer 호출 횟수 = 1 |
| 9 | KeepAliveManager | pong timeout 3회 | OnSessionClosed 호출 |
| 10 | KeepAliveManager | OnBytesReceived 호출 → idle 재시작 | ping 미발사 (5s 지연) |
| 11 | ServerLifecycle | Shutdown 순서 | TimerQueue.Shutdown 호출 시각 < Logger.Shutdown 호출 시각 (mock 기록) |
| 12 | ServerLifecycle | drain timeout 초과 | force join + E_SHUTDOWN_TIMEOUT 로그 |
| 13 | Buffer / BufferOps | CopyToHeap(span) | 동일 내용, 독립 포인터 |
| 14 | Buffer / BufferOps | ToShared(span) | shared_ptr use_count == 1, 내용 일치 |

### 8.3 L2 — Integration Scenarios

| # | 시나리오 | 기대 |
|---|---|---|
| 1 | in-proc echo 1 round-trip | 성공 |
| 2 | 1K 세션 × 100 echo 동시 | 0 crash, 0 leak |
| 3 | KeepAlive 타임아웃 발생 | 해당 세션만 close, 나머지 유지 |
| 4 | Shutdown 호출 중 신규 connect 시도 | 거부 (Draining state) |
| 5 | OnPacket 에서 예외 throw | 해당 세션 close, 서버 계속 동작 |

### 8.4 L3 — E2E Scenarios (sample)

| # | 시나리오 | 기대 |
|---|---|---|
| 1 | clone → build → echo-server + echo-client 1 packet | CI PowerShell 스크립트 timed=10분 이내 |
| 2 | chat 샘플 3클라이언트 broadcast | 모든 클라이언트 동일 메시지 수신 |
| 3 | 샘플 README 의 Troubleshooting 재현 | 각 문제 해결 스텝 동작 |

### 8.5 L4 — Benchmark Scenarios

| # | 시나리오 | 기준 |
|---|---|---|
| 1 | 64B 단일 세션 echo Peak PPS (로컬) | ≥ 30,000 |
| 2 | 64B echo P50/P99 (로컬) | P50 ≤ 30µs, P99 ≤ 80µs |
| 3 | 1K 세션 × 10 pps sustained (로컬) | 0 drop, CPU ≤ 80% |
| 4 | 동일 시나리오 (클라우드 지정 인스턴스) | 결과 markdown 포함, 로컬과 range 비교 표 |

### 8.6 L5 — Stress / Lifetime

| # | 시나리오 | 기준 |
|---|---|---|
| 1 | lifetime race reproducer 1M×2 | crash=0, leak=0 (CRT debug alloc 체크) |
| 2 | concurrent close × recv / close × send / close × timer | state machine 에서 catch, 0 crash |
| 3 | Graceful shutdown 중 1K 세션 동시 close | 순서 역전 없음, Logger/TimerQueue 잔존 callback 0 |

### 8.7 Seed Data

| Entity | Minimum | Required Fields |
|--------|:-:|---|
| `ServerConfig` (test) | 1 | port=0 (auto), workers=2, maxSessions=100 |
| `KeepAliveConfig` (test) | 1 | idleBeforePing=100ms, pongTimeout=50ms (고속 검증용) |
| Dummy packets | 10000 | length 1~64B random |

---

## 9. Clean Architecture

### 9.1 Layer Structure (C++ 프로필)

| Layer | 역할 | 위치 |
|---|---|---|
| **Application** | 샘플/서버 실행형 진입점 | `samples/echo/*`, `samples/chat/*`, `FastPortServer/main.cpp` |
| **Domain** (Engine Core) | IOCP 엔진 로직 + 공개 API | `LibNetworks/` (신규 모듈 포함) |
| **Infrastructure (shared)** | Logger, TimerQueue, Buffer, RWLock | `LibCommons/` |
| **Test** | 유닛/통합/스트레스 | `LibNetworksTests/`, `LibCommonsTests/` |
| **Bench / Samples** | 재현 가능 성능 검증 + 레퍼런스 | `FastPortBenchmark/`, `benchmark/`, `samples/*` |
| **Freeze** | v1 동안 변경 금지 | `LibNetworksRIO/`, `FastPortServerRIO/`, `LibNetworksRIOTests/` |

### 9.2 Dependency Rules

```
┌──────────────────────────────────────────────────────────┐
│  Application (samples, FastPortServer)                   │
│         │                                                │
│         ▼                                                │
│  Domain (LibNetworks = IOCP engine)                      │
│         │                                                │
│         ▼                                                │
│  Infrastructure (LibCommons)                             │
│                                                          │
│  Rule: inner layers MUST NOT depend on outer layers      │
│  Rule: LibCommons has NO dependency on LibNetworks       │
│  Rule: new modules MUST NOT import LibNetworksRIO        │
└──────────────────────────────────────────────────────────┘
```

### 9.3 File Import Rules

| From | Can Import | Cannot Import |
|---|---|---|
| `samples/*`, `FastPortServer` | `LibNetworks`, `LibCommons`, `Protocols` | `LibNetworksRIO` (freeze) |
| `LibNetworks` (new modules) | `LibCommons`, OS headers | `LibNetworksRIO`, application code |
| `LibCommons` | OS headers, spdlog | `LibNetworks*`, application |
| `LibNetworksRIO` | (freeze) 기존 import 유지, 신규 import 금지 | — |

### 9.4 Feature → Layer Assignment

| Component | Layer | Location |
|---|---|---|
| `RecvBufferPool` | Domain | `LibNetworks/RecvBufferPool.{ixx,cpp}` |
| `KeepAliveManager` | Domain | `LibNetworks/KeepAliveManager.{ixx,cpp}` |
| `ServerLifecycle` | Domain | `LibNetworks/ServerLifecycle.{ixx,cpp}` |
| `PacketFramerCore` | Domain | `LibNetworks/PacketFramerCore.{ixx,cpp}` |
| `LengthPrefixPolicy` + FramingPolicy concept | Domain | `LibNetworks/FramingPolicy.{ixx,cpp}` |
| `Buffer`, `BufferOps` | Infrastructure (shared) | `LibCommons/Buffer.{ixx,cpp}` |
| `IOSession` (modified) | Domain | `LibNetworks/IOSession.{ixx,cpp}` |
| Echo sample | Application | `samples/echo/` |
| Chat sample | Application | `samples/chat/` |
| Bench scripts | (Tooling) | `benchmark/` |

---

## 10. Coding Convention Reference

> Primary source: `CLAUDE.md` (프로젝트 루트).

### 10.1 Naming Conventions (본 프로젝트)

| Target | Rule | Example |
|---|---|---|
| Class/Struct/Method | PascalCase | `RecvBufferPool`, `KeepAliveManager::OnSessionOpened()` |
| Member | `m_` + PascalCase | `m_State`, `m_pSlot`, `m_bDraining` |
| Static/const | `k` + PascalCase | `kRecvSlotSize`, `kLogCategoryLife` |
| Win32 API | `::` 전역 | `::CreateThreadpoolTimer`, `::GetLastError` |
| Module 이름 | `commons.snake_case` / `networks.snake_case` | `commons.buffer`, `networks.recv_buffer_pool` |

### 10.2 Module / Import Order (GMF)

```cpp
module;
#include <Windows.h>
#include <spdlog/spdlog.h>   // Logger 사용 모듈 필수 (C1001 ICE 회피, CLAUDE.md)
#include <MSWSock.h>         // IOCP 계열 모듈만
export module networks.recv_buffer_pool;
import std;
import commons.logger;
import commons.buffer;
```

### 10.3 Logger 사용 규약

```cpp
namespace {
constexpr const char* kLogCategory = "RecvBufferPool";
inline void LogInfoX (const std::string& m){ FastPort::LibCommons::Logger::GetInstance().LogInfo (kLogCategory, m); }
inline void LogWarnX (const std::string& m){ FastPort::LibCommons::Logger::GetInstance().LogWarning(kLogCategory, m); }
inline void LogErrorX(const std::string& m){ FastPort::LibCommons::Logger::GetInstance().LogError(kLogCategory, m); }
inline void LogDebugX(const std::string& m){ FastPort::LibCommons::Logger::GetInstance().LogDebug(kLogCategory, m); }
}
// 호출:
LogErrorX(std::format("crossing queue overflow (worker={}, size={})", wid, sz));
```

### 10.4 Mutex / Lock

- Hot path (RecvBufferPool, PacketFramerCore): **lock-free**. TLS + MPSC queue.
- Cold path (KeepAliveManager state, ServerLifecycle transitions): `std::mutex` + `std::lock_guard`
- Session registry 등 multi-reader: `LibCommons::RWLock` + `ReadLockBlock` / `WriteLockBlock`

---

## 11. Implementation Guide

### 11.1 File Structure

```
LibCommons/
  Buffer.ixx              (new — value type + ops namespace)
  Buffer.cpp              (new)
  Logger.*                (existing)
  TimerQueue.*            (existing, lifetime race fix feature 참조)
  RWLock.*                (existing)

LibNetworks/
  RecvBufferPool.ixx      (new)
  RecvBufferPool.cpp      (new)
  FramingPolicy.ixx       (new — concept + LengthPrefixPolicy)
  FramingPolicy.cpp       (new)
  PacketFramerCore.ixx    (new — or refactor existing PacketFramer)
  PacketFramerCore.cpp    (new)
  KeepAliveManager.ixx    (new)
  KeepAliveManager.cpp    (new)
  ServerLifecycle.ixx     (new)
  ServerLifecycle.cpp     (new)
  IOSession.*             (modified — Recv path → pool, OnRecvComplete → framer core,
                           race fix 병합)
  Acceptor.*              (modified — Shutdown 시 정지 훅)
  Admin/ServerStatus.*    (modified — pool/keepalive stats 필드 추가)

LibNetworksTests/
  RecvBufferPool.test.cpp (new)
  FramingPolicy.test.cpp  (new)
  PacketFramerCore.test.cpp (new)
  KeepAliveManager.test.cpp (new)
  ServerLifecycle.test.cpp (new)
  LifetimeRaceStress.test.cpp (enhanced — 1M×2 round + crossing cases)
  Integration.test.cpp    (new — in-proc server+client)

LibCommonsTests/
  Buffer.test.cpp         (new)

samples/
  echo/
    server/main.cpp       (new)
    client/main.cpp       (new)
    README.md             (new)
  chat/
    server/main.cpp       (new)
    client/main.cpp       (new)
    README.md             (new)

benchmark/
  bench_echo.ps1          (new — 로컬)
  bench_echo_cloud.ps1    (new — 원격 실행 wrapper)
  ENV_TEMPLATE.md         (new — 결과 markdown 환경 블록)

docs/
  ARCHITECTURE_IOCP.md    (rewritten to v2, 한국어)
  benchmark-results-*.md  (기존 3개 → docs/archive/benchmarks-v0/ 로 이동 후 v1 결과 교체)
```

### 11.2 Implementation Order

1. [ ] **M0** (사전 조건): `iosession-lifetime-race` feature 의 Option C Pragmatic 병합 완료 확인
2. [ ] **M2a** `LibCommons/Buffer` 값 타입 + BufferOps (의존성 없음 → 먼저)
3. [ ] **M2b** `LibNetworks/RecvBufferPool` (TLS + crossing queue) + 유닛 테스트
4. [ ] **M2c** `LibNetworks/FramingPolicy` (concept + LengthPrefixPolicy) + 유닛 테스트
5. [ ] **M2d** `LibNetworks/PacketFramerCore` (IOSession 통합 전 단계) + 유닛 테스트
6. [ ] **M2e** `IOSession` Recv 경로를 RecvBufferPool + PacketFramerCore 로 교체 (race fix 전제)
7. [ ] **M3** `LibNetworks/KeepAliveManager` + 유닛 테스트 + IOSession 통합
8. [ ] **M4** `LibNetworks/ServerLifecycle` + 순서 유닛 테스트 + `FastPortServer` 이관
9. [ ] **M5a** `samples/echo` + README + timed first-run 검증
10. [ ] **M5b** `samples/chat` + README
11. [ ] **M5c** `benchmark/bench_echo.ps1` + `ENV_TEMPLATE.md` (로컬 결과 생성)
12. [ ] **M5d** `benchmark/bench_echo_cloud.ps1` (클라우드 vm 원격) + 결과 병합
13. [ ] **M5e** 기존 `docs/benchmark-results-*.md` → `docs/archive/benchmarks-v0/` 이동
14. [ ] **M6** `docs/ARCHITECTURE_IOCP.md` v2 재작성 (4 다이어그램 + 한국어)
15. [ ] 전 구간 L5 stress 1M×2 재검증 → `/pdca analyze`

### 11.3 Session Guide

> 장기 구현을 위한 모듈 단위 세션 분할. `/pdca do iocp-game-server-engine --scope <key>` 로 하나씩.

#### Module Map

| Module | Scope Key | 설명 | 예상 턴수 |
|---|---|---|:-:|
| Buffer 값 타입 + BufferOps | `m2a-buffer` | `LibCommons/Buffer.*` + unit tests | 15-20 |
| RecvBufferPool | `m2b-pool` | TLS slab + crossing queue + unit tests | 30-40 |
| FramingPolicy | `m2c-framing` | concept + LengthPrefixPolicy + unit tests | 15-20 |
| PacketFramerCore | `m2d-framer` | span 콜백 dispatch + carry-over + unit tests | 25-35 |
| IOSession Recv 통합 | `m2e-iosession` | Recv path 교체 (race fix 전제) | 30-40 |
| KeepAliveManager | `m3-keepalive` | App ping/pong + TCP keepalive + tests | 25-35 |
| ServerLifecycle | `m4-lifecycle` | state machine + 순서 잠금 + tests | 25-35 |
| samples/echo + README | `m5a-sample-echo` | 2 실행형 + README + timed run | 20-30 |
| samples/chat + README | `m5b-sample-chat` | broadcast 샘플 | 20-30 |
| benchmark 로컬 | `m5c-bench-local` | ps1 + ENV_TEMPLATE + 로컬 결과 | 20-30 |
| benchmark 클라우드 | `m5d-bench-cloud` | 원격 실행 + 결과 병합 | 25-35 |
| 벤치 결과 이관 | `m5e-bench-archive` | 기존 파일 archive 이동 | 5-10 |
| ARCHITECTURE_IOCP.md v2 | `m6-docs` | 한국어 + 4 다이어그램 | 20-30 |

#### Recommended Session Plan

| Session | Phase | Scope | 턴수 |
|---|---|---|:-:|
| S1 (done) | PM + Plan + Design | 전체 | 35 |
| S2 | Do | `--scope m2a-buffer,m2b-pool` | 40-50 |
| S3 | Do | `--scope m2c-framing,m2d-framer` | 40-50 |
| S4 | Do | `--scope m2e-iosession` | 40-50 |
| S5 | Do | `--scope m3-keepalive,m4-lifecycle` | 50-60 |
| S6 | Do | `--scope m5a-sample-echo,m5b-sample-chat` | 40-50 |
| S7 | Do | `--scope m5c-bench-local,m5d-bench-cloud,m5e-bench-archive` | 50-60 |
| S8 | Do | `--scope m6-docs` | 25-35 |
| S9 | Check + Iterate (필요시) | 전체 gap-detector | 30-50 |
| S10 | QA + Report | 전체 | 30-40 |

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-04-22 | Initial draft (Option B — Clean Architecture 선택. 4 신규 Domain 모듈 + 1 Infrastructure 모듈. M1~M6 매핑, Session Guide 13 scope) | An Younggun |
