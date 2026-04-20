# session-idle-timeout Design Document

> **Summary**: `IIdleAware` 인터페이스 + `SessionIdleChecker` (non-template) + `Container::ForEach/Snapshot`. IOCP 세션부터 도입, RIO 는 동일 패턴 후속.
>
> **Project**: FastPort
> **Author**: AnYounggun
> **Date**: 2026-04-20
> **Status**: Draft
> **Planning Doc**: [session-idle-timeout.plan.md](../../01-plan/features/session-idle-timeout.plan.md)

---

## Context Anchor

> Copied from Plan.

| Key | Value |
|-----|-------|
| **WHY** | 비정상 단절·freeze 미감지로 유령 세션 축적, 상태 불일치, RIO 버퍼 릭 위험 |
| **WHO** | FastPort 엔진 서버 운영자 / 매칭 로직 개발자 / 클라이언트 개발자 |
| **RISK** | 콜백에서 세션 맵 순회 중 race, 정상 트래픽 오탐, 타이머-워커 간 동기화, 대량 동시 timeout 부하 스파이크 |
| **SUCCESS** | 비정상 단절 감지 ≤ 11s(기본), IOCP 구현 후 동일 패턴 RIO 적용 가능, 정상 세션 오탐 0, 리소스 릭 0 |
| **SCOPE** | Phase 1 — FastPortServer(IOCP) + LibNetworks 공용. Phase 2 — Ping/Pong. RIO 는 후속 |

---

## 1. Overview

### 1.1 Design Goals

1. **타입 불변 IdleChecker**: 세션 구체 타입(IOCP/RIO/Outbound)을 모르는 checker. 인터페이스 `IIdleAware` 에만 의존.
2. **스냅샷 기반 안전한 순회**: Container 에 `ForEach/Snapshot` 추가. 락 경합 최소화 + 순회 중 mutation 회피.
3. **기존 흐름 보존**: `OnDisconnected` 시그니처 유지, 기존 `RequestDisconnect` CAS 재사용.
4. **최소 침습**: 세션 타입마다 재작성 없음. `IOSession` 1곳에서 `IIdleAware` 구현.
5. **계측 친화**: Logger 카테고리 `"IdleChecker"` 로 disconnect 사유/세션 id/idle duration 출력.

### 1.2 Design Principles

- **Interface Segregation**: `IIdleAware` 는 2개 메서드(`GetLastRecvTimeMs`, `RequestDisconnect(reason)`)만 정의
- **Dependency Inversion**: IdleChecker 는 추상(`IIdleAware`) 에 의존, 구체 세션에 비의존
- **Snapshot over Iteration**: tick 콜백에서 락 잡고 직접 순회하지 않고 `std::vector<shared_ptr<IIdleAware>>` 스냅샷 → 락 풀고 처리
- **Fail Safe**: 콜백 내부 예외 catch-all, Shutdown 시 현재 tick 완료 대기 (TimerQueue Cancel 의 Wait path 재사용)
- **Log Before Act**: disconnect 직전 로그 — 사후 디버깅 가능

---

## 2. Architecture Options

### 2.0 Comparison

| Criteria | A: Minimal | **B: Clean (Selected)** | C: Pragmatic |
|----------|:-:|:-:|:-:|
| Container 확장 | 없음 (FindIf 부수효과) | `Snapshot()` + `ForEach()` | `ForEach()` 1개 |
| 새 추상화 | 없음 | **`IIdleAware` 인터페이스** | 없음 (template) |
| IdleChecker 형태 | template | **non-template (IIdleAware*)** | template |
| 복잡도 | Low | High | Medium |
| 유지보수성 | Medium | **High** | High |
| LOC | ~200 | **~400** | ~250 |

**Selected: Option B (Clean Architecture)** — **Rationale**:
- 향후 RIO, Outbound, 다른 서브시스템(관리자 연결 등) 모두 `IIdleAware` 만 구현하면 동일 IdleChecker 재사용
- 템플릿 매개화보다 인터페이스가 **컴파일 시간/바이너리 크기** 유리
- 테스트 용이: mock IIdleAware 로 IdleChecker 독립 테스트 가능
- `ForEach` 는 read-lock 유지 순회, `Snapshot` 은 vector 복사 — 용도에 따라 선택 가능

### 2.1 Component Diagram

```
┌──────────────────────────────────────────────────────────────────────────┐
│ Consumer Layer: FastPortServer / IOCPServiceMode                         │
│   - 앱 생명주기 동안 SessionIdleChecker 소유                                 │
│   - snapshotProvider 람다로 SessionContainer 를 IIdleAware 리스트로 변환    │
└──────────────────────────────────────────────────────────────────────────┘
                          │
                          │ make_shared + Start()
                          ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ networks.sessions.idle_checker                                           │
│   class SessionIdleChecker                                               │
│     - IdleCheckerConfig (thresholdMs, tickIntervalMs, enabled)            │
│     - SnapshotProvider: std::function<std::vector<shared_ptr<IIdleAware>>()> │
│     - TimerId (TimerQueue::GetInstance() 에 SchedulePeriodic)             │
│     - OnTick(): snapshot → 각 IIdleAware 검사 → RequestDisconnect         │
└──────────────────────────────────────────────────────────────────────────┘
                          │
                          │ uses interface only
                          ▼
┌──────────────────────────────────────────────────────────────────────────┐
│ networks.sessions.iidle_aware                                            │
│   struct IIdleAware                                                      │
│     - GetLastRecvTimeMs() const noexcept                                 │
│     - RequestDisconnect(DisconnectReason reason)                         │
└──────────────────────────────────────────────────────────────────────────┘
                          ▲
                          │ implements
                          │
┌──────────────────────────────────────────────────────────────────────────┐
│ networks.sessions.io_session                                             │
│   class IOSession : ..., public IIdleAware                               │
│     - std::atomic<int64_t> m_LastRecvTimeMs                              │
│     - OnIOCompleted(): 수신(bytes > 0) 시 steady_clock::now() ms 갱신     │
│     - RequestDisconnect(reason) 오버로드 — 기존 protected 함수에 reason 파라미터 │
└──────────────────────────────────────────────────────────────────────────┘

                   ┌──────────────────────────┐
                   │ commons.container        │
                   │   (확장 2개)              │
                   │   - ForEach(cb)          │   read-lock 유지, 콜백 순회
                   │   - Snapshot() -> vector │   복사 후 반환
                   └──────────────────────────┘
```

### 2.2 Data Flow

#### 정상 트래픽 (오탐 방지)

```
Client send → 서버 recv
    │
    ▼
IOSession::OnIOCompleted(bytes > 0)
    │ m_LastRecvTimeMs.store(NowMs(), relaxed)
    │ OnPacketReceived 등 기존 경로 진행
    ▼
[이후 tick]
    │
    ▼
IdleChecker::OnTick()
    │ now - last < thresholdMs → skip
    ▼ (세션 유지)
```

#### 비정상 단절 감지

```
Client 프로세스 강제 종료 (FIN 누락)
    │
    ▼
서버 recv 중단 — OS Keep-Alive 까지 수초~수십초 공백
    │
    │ … thresholdMs + tickIntervalMs 경과 …
    │
    ▼
IdleChecker::OnTick() [TimerQueue 워커 스레드]
    │ provider() → shared_ptr<IIdleAware> 리스트 스냅샷
    │ 각 세션: now - last >= thresholdMs → 대상 선별
    ▼
IIdleAware::RequestDisconnect(IdleTimeout)
    │
    ▼
IOSession::RequestDisconnect(IdleTimeout)
    │ Logger "Disconnecting session {id}, reason=IdleTimeout, idleMs={elapsed}"
    │ m_DisconnectRequested CAS — 이중 호출 방지
    │ Socket::Close → OnIOCompleted(error) → OnDisconnected()
    ▼
IOCPInboundSession::OnDisconnected()
    │ SessionContainer::Remove(id)  (기존 흐름)
    ▼
shared_ptr refcount → 0 → 세션 소멸
```

#### Shutdown 시퀀스

```
IOCPServiceMode::OnStopped()
    │
    ▼
m_IdleChecker->Stop()
    │ m_Running = false
    │ TimerQueue::Cancel(m_TimerId)  — Wait path: 진행 중 tick 완료까지 대기
    ▼
m_IdleChecker 소멸 안전
```

### 2.3 Dependencies

| Component | Depends On | Purpose |
|-----------|-----------|---------|
| `SessionIdleChecker` | `IIdleAware`, `LibCommons::TimerQueue`, `LibCommons::Logger` | 타이머 tick + 로깅 |
| `IOSession` | `IIdleAware` | 인터페이스 구현 |
| `INetworkSession` | `DisconnectReason` enum | 공통 enum |
| `IIdleAware` | `DisconnectReason` | 메서드 시그니처 |
| `Container::ForEach/Snapshot` | `RWLock` | 스레드 안전 순회 |
| Consumer (IOCPServiceMode) | `SessionIdleChecker` + `SessionContainer` | 조합자 |

---

## 3. Data Model

### 3.1 DisconnectReason Enum

```cpp
// LibNetworks/INetworkSession.ixx 에 추가
export namespace LibNetworks::Sessions {
    export enum class DisconnectReason : std::uint8_t {
        Normal        = 0,   // 일반 요청 / graceful close
        IdleTimeout   = 1,   // SessionIdleChecker 가 감지
        Backpressure  = 2,   // 송신 큐 임계 초과 (RIOSession.cpp 에서 이미 사용 가능)
        Protocol      = 3,   // 프로토콜 위반
        Server        = 4,   // 서버 측 명시 종료 (관리자/셧다운)
    };
}
```

### 3.2 IIdleAware Interface

```cpp
// LibNetworks/IIdleAware.ixx (신규)
module;
#include <cstdint>
export module networks.sessions.iidle_aware;

import networks.sessions.inetwork_session;  // DisconnectReason

namespace LibNetworks::Sessions {
    // Thread-safety: 구현체는 두 메서드가 concurrent call 가능해야 함.
    //   GetLastRecvTimeMs: atomic read (lock-free 권장)
    //   RequestDisconnect: 내부 이중 호출 방지 필수 (m_DisconnectRequested CAS)
    export struct IIdleAware {
        virtual ~IIdleAware() = default;

        // 마지막 수신 시각 (steady_clock 기준 epoch-ms). 0 이면 아직 수신 없음.
        virtual std::int64_t GetLastRecvTimeMs() const noexcept = 0;

        // 지정한 사유로 연결 종료 요청. 이중 호출은 구현체에서 무시.
        virtual void RequestDisconnect(DisconnectReason reason) = 0;
    };
}
```

### 3.3 IdleCheckerConfig

```cpp
export struct IdleCheckerConfig {
    std::chrono::milliseconds thresholdMs    { 10000 };  // idle 간주 임계
    std::chrono::milliseconds tickIntervalMs { 1000 };   // 주기 검사 간격
    bool                      enabled        { true };   // false 면 tick 등록 안 함
};
```

### 3.4 Internal State

```cpp
// SessionIdleChecker.cpp 내부
class SessionIdleChecker {
    IdleCheckerConfig                       m_Config;
    SnapshotProvider                        m_Provider;      // std::function<vector<shared_ptr<IIdleAware>>()>
    LibCommons::TimerId                     m_TimerId { kInvalidTimerId };
    std::atomic<bool>                       m_Running { false };
};
```

---

## 4. API Specification

### 4.1 Container 확장 (`LibCommons/Container.ixx`)

```cpp
// 추가: 읽기 락 유지 중 각 엔트리 처리.
// NOTE: 콜백 내부에서 Container::Add/Remove 호출 금지 (read→write 재진입 데드락).
template<typename Fn>
void ForEach(Fn&& fn) const {
    auto lock = ReadLockBlock(m_Lock);
    for (auto const& [k, v] : m_Storage) {
        fn(k, v);
    }
}

// 추가: 값 복사로 스냅샷 반환. 콜백 제약 없이 안전하게 외부에서 처리 가능.
std::vector<std::pair<Key, T>> Snapshot() const {
    auto lock = ReadLockBlock(m_Lock);
    std::vector<std::pair<Key, T>> result;
    result.reserve(m_Storage.size());
    for (auto const& [k, v] : m_Storage) {
        result.emplace_back(k, v);
    }
    return result;
}
```

### 4.2 IOSession 확장 (`LibNetworks/IOSession.ixx`)

```cpp
export class IOSession : public Core::IIOConsumer,
                         public INetworkSession,
                         public IIdleAware,                    // NEW
                         public std::enable_shared_from_this<IOSession>
{
public:
    // ... 기존 API ...

    // IIdleAware 구현
    std::int64_t GetLastRecvTimeMs() const noexcept override {
        return m_LastRecvTimeMs.load(std::memory_order_relaxed);
    }
    void RequestDisconnect(DisconnectReason reason) override;  // public 승격

    // 기존 호출자 호환: reason=Normal 로 위임
    void RequestDisconnect();  // (기존 protected → public 승격 OR 기존 시그니처 유지 후 내부 delegate)

protected:
    // OnIOCompleted 에서 수신 성공 시 갱신
    std::atomic<std::int64_t> m_LastRecvTimeMs { 0 };
};
```

### 4.3 SessionIdleChecker API (`LibNetworks/SessionIdleChecker.ixx`)

```cpp
module;
#include <functional>
export module networks.sessions.idle_checker;

import std;
import networks.sessions.iidle_aware;

namespace LibNetworks::Sessions {

export struct IdleCheckerConfig {
    std::chrono::milliseconds thresholdMs    { 10000 };
    std::chrono::milliseconds tickIntervalMs { 1000 };
    bool                      enabled        { true };
};

// Thread-safety: Start/Stop 은 단일 스레드(소유자) 에서 호출. tick 콜백은 TimerQueue 워커.
export class SessionIdleChecker
{
public:
    using SnapshotProvider = std::function<std::vector<std::shared_ptr<IIdleAware>>()>;

    SessionIdleChecker(IdleCheckerConfig cfg, SnapshotProvider provider);
    ~SessionIdleChecker();

    SessionIdleChecker(const SessionIdleChecker&)            = delete;
    SessionIdleChecker& operator=(const SessionIdleChecker&) = delete;

    // TimerQueue 에 tick 스케줄. enabled=false 면 no-op.
    void Start();

    // tick 취소 + 진행 중 콜백 완료 대기. 재호출은 idempotent.
    void Stop();

    const IdleCheckerConfig& GetConfig() const noexcept { return m_Config; }

private:
    void OnTick();

    IdleCheckerConfig          m_Config;
    SnapshotProvider           m_Provider;
    LibCommons::TimerId        m_TimerId  { LibCommons::kInvalidTimerId };
    std::atomic<bool>          m_Running  { false };
};

} // namespace LibNetworks::Sessions
```

### 4.4 소비자 사용 예 (`FastPortServer/IOCPServiceMode.cpp`)

```cpp
// OnStarted() 내부 — Acceptor 시작 후
using SessionContainer = LibCommons::Container<uint64_t,
    std::shared_ptr<LibNetworks::Sessions::InboundSession>>;

m_IdleChecker = std::make_shared<LibNetworks::Sessions::SessionIdleChecker>(
    LibNetworks::Sessions::IdleCheckerConfig{
        .thresholdMs    = std::chrono::milliseconds(10'000),
        .tickIntervalMs = std::chrono::milliseconds(1'000),
        .enabled        = true,
    },
    []() -> std::vector<std::shared_ptr<LibNetworks::Sessions::IIdleAware>> {
        std::vector<std::shared_ptr<LibNetworks::Sessions::IIdleAware>> out;
        auto& container = LibCommons::SingleTon<SessionContainer>::GetInstance();
        container.ForEach([&out](auto const& /*k*/, auto const& pSession) {
            if (pSession) {
                out.push_back(std::static_pointer_cast<LibNetworks::Sessions::IIdleAware>(pSession));
            }
        });
        return out;
    });

m_IdleChecker->Start();

// OnStopped() 내부
if (m_IdleChecker) {
    m_IdleChecker->Stop();
    m_IdleChecker.reset();
}
```

---

## 5. UI/UX Design

해당 없음 (백엔드 인프라). 로그 가시성은 §6 참조.

---

## 6. Error Handling

### 6.1 Error Scenarios

| # | 상황 | 동작 | Logger |
|---|------|------|--------|
| 1 | Start 호출 시 `enabled=false` | no-op, TimerId 은 Invalid 유지 | Info: "IdleChecker", "Disabled, skip scheduling" |
| 2 | Start 이중 호출 | 기존 tick 유지, 재등록 안 함 | Info: "IdleChecker", "Already running" |
| 3 | Provider 가 빈 vector 반환 | tick 정상 실행, disconnect 대상 없음 | (로그 없음) |
| 4 | Provider 내부에서 예외 | OnTick 에서 catch, 다음 tick 유지 | Error: "IdleChecker", "Snapshot provider threw: {what}" |
| 5 | `IIdleAware::GetLastRecvTimeMs` 가 0 반환 | 아직 수신 이력 없음 → skip (방금 연결됐고 첫 패킷 안 옴) | (로그 없음) |
| 6 | `RequestDisconnect(IdleTimeout)` 이 예외 throw | OnTick catch, 다음 세션 계속 | Error: "IdleChecker", "RequestDisconnect threw for session {id}: {what}" |
| 7 | Stop 중복 호출 | idempotent, 이미 정리됨 | (로그 없음) |
| 8 | 소멸자 호출 시 Start 안 됐거나 이미 Stop 됨 | 안전한 소멸 | (로그 없음) |

### 6.2 OnTick 의 예외 정책

```cpp
void SessionIdleChecker::OnTick() {
    if (!m_Running.load(std::memory_order_acquire)) return;

    std::vector<std::shared_ptr<IIdleAware>> snapshot;
    try {
        snapshot = m_Provider();
    } catch (const std::exception& e) {
        LibCommons::Logger::GetInstance().LogError("IdleChecker",
            std::format("Snapshot provider threw: {}", e.what()));
        return;
    }

    const auto nowMs = NowMs();
    const auto thresholdMs = m_Config.thresholdMs.count();

    for (auto& pSession : snapshot) {
        if (!pSession) continue;
        const auto last = pSession->GetLastRecvTimeMs();
        if (last == 0) continue;  // 수신 이력 없음
        const auto elapsed = nowMs - last;
        if (elapsed < thresholdMs) continue;

        try {
            pSession->RequestDisconnect(DisconnectReason::IdleTimeout);
            // 로그는 IOSession::RequestDisconnect(reason) 에서 출력 (session id 접근 용이)
        } catch (const std::exception& e) {
            LibCommons::Logger::GetInstance().LogError("IdleChecker",
                std::format("RequestDisconnect threw: {}", e.what()));
        }
    }
}
```

### 6.3 Logging Contract

| 시점 | Level | Message |
|---|---|---|
| `Start()` 성공 | Info | "IdleChecker started. thresholdMs={}, tickIntervalMs={}" |
| `Start()` disabled | Info | "IdleChecker disabled, skip scheduling" |
| `Stop()` | Info | "IdleChecker stopped" |
| Idle 감지 | Info | "IdleTimeout detected. Session Id : {id}, IdleMs : {elapsed}, Threshold : {threshold}" *(IOSession::RequestDisconnect 내부)* |
| Provider/RequestDisconnect 예외 | Error | 위 표 참조 |

---

## 7. Security Considerations

- **DoS 방지**: idle 임계 너무 짧으면 일시적 네트워크 지연도 강제 종료 → 기본 10s 는 보수적
- **조작 방지**: `m_LastRecvTimeMs` 는 서버 내부 atomic 변수, 클라이언트가 직접 쓸 수 없음
- **로그 누출 방지**: session id 외의 개인정보(IP 등) 를 idle 로그에 포함하지 않음
- **Provider 의 범위**: provider 람다가 SessionContainer 참조를 오래 유지하지 않도록 호출마다 SingleTon 조회 (라이프사이클 관리 명확화)

---

## 8. Test Plan

### 8.1 Test Scope

| Type | Target | Tool |
|------|--------|------|
| L1 Unit | `Container::ForEach/Snapshot` | `LibCommonsTests` (CppUnitTest) |
| L1 Unit | `SessionIdleChecker` 타이밍/로직 (Mock IIdleAware) | `LibNetworksTests` (CppUnitTest) |
| L2 Integration | `IOSession::m_LastRecvTimeMs` 실제 수신 경로에서 갱신 | `LibNetworksTests` |
| L3 Scenario | IOCP 서버 실행 + TestClient 강제 종료 → idle timeout 로그/종료 확인 | 수동 |

### 8.2 L1 — Unit Tests (`LibCommonsTests/ContainerTests.cpp` 추가)

| # | Test | Assertion |
|---|------|-----------|
| C-01 | `ForEach_EmptyContainer` | 콜백 호출 0회 |
| C-02 | `ForEach_Iterates_AllEntries` | 3 엔트리 Add 후 ForEach → 3회 호출, key/value 모두 조회됨 |
| C-03 | `Snapshot_ReturnsCopy` | Add 후 Snapshot → size 일치. 이후 Remove 해도 이전 snapshot 변하지 않음 |
| C-04 | `ForEach_ReadLock_AllowsConcurrentReads` | 2 스레드 동시 ForEach → 데드락 없음 |
| C-05 | `ForEach_CallbackMustNotMutate` *(문서 제약 확인)* | 가이드 주석 존재 확인 |

### 8.3 L1 — SessionIdleChecker Unit (`LibNetworksTests/SessionIdleCheckerTests.cpp` 신규)

Mock `IIdleAware`:
```cpp
struct MockIdleAware : public LibNetworks::Sessions::IIdleAware {
    std::atomic<std::int64_t> lastRecvMs { 0 };
    std::atomic<int>          disconnectCount { 0 };
    std::atomic<LibNetworks::Sessions::DisconnectReason> lastReason{};

    std::int64_t GetLastRecvTimeMs() const noexcept override { return lastRecvMs.load(); }
    void RequestDisconnect(LibNetworks::Sessions::DisconnectReason reason) override {
        disconnectCount.fetch_add(1);
        lastReason.store(reason);
    }
};
```

| # | Test | Setup | Assertion |
|---|------|-------|-----------|
| I-01 | `Start_WithEnabledFalse_NeverTicks` | `enabled=false`, 2 mock | 500ms 대기 후 disconnectCount = 0 |
| I-02 | `Start_WithFreshSession_NoDisconnect` | threshold=500ms, tick=50ms, mock.lastRecvMs=NowMs() 주기적 갱신 | 1000ms 대기 후 disconnectCount = 0 (정상 트래픽 유지) |
| I-03 | `Start_WithStaleSession_DisconnectsWithinBudget` | threshold=200ms, tick=50ms, lastRecvMs=고정(1000ms 전) | 500ms 내 disconnectCount ≥ 1, lastReason=IdleTimeout |
| I-04 | `MultipleSessions_OnlyStaleDisconnected` | 3 mock: 1개 stale, 2개 fresh | stale 만 disconnect, fresh 2개는 count=0 |
| I-05 | `LastRecvZero_Skipped` | mock.lastRecvMs=0 (아직 수신 없음), threshold=10ms | 100ms 대기 후 disconnectCount = 0 |
| I-06 | `Stop_PreventsFurtherTicks` | 스케줄 후 즉시 Stop | Stop 이후 disconnectCount 증가 없음 |
| I-07 | `ProviderException_LoggedNotFatal` | provider 가 throw | checker 가 살아있음, 다음 tick 정상 |
| I-08 | `RequestDisconnectException_LoggedContinues` | mock 의 RequestDisconnect 가 throw | 다른 mock 은 정상 처리됨 |

### 8.4 L2 — IOSession lastRecvTime Integration

| # | Test | Assertion |
|---|------|-----------|
| L2-01 | `IOSession_LastRecvTime_UpdatedOnReceive` | Mock 서버 ↔ Mock 클라이언트 loopback, 클라이언트가 1 byte 전송 후 lastRecvTimeMs > 0 |
| L2-02 | `IOSession_LastRecvTime_NotUpdatedOnZeroByte` | Zero-byte Recv 완료는 lastRecv 갱신 안 함 (수신 이력 아님) |
| L2-03 | `IOSession_RequestDisconnectReason_Logged` | `RequestDisconnect(IdleTimeout)` 호출 → 로그에 "reason=IdleTimeout" 포함 |

### 8.5 L3 — Manual Scenario (FastPortServer)

| # | 시나리오 | 기대 결과 |
|---|----------|-----------|
| M-01 | FastPortServer 실행 → TestClient 연결 → 활발 송수신 | 30초 이상 유지, idle log 없음 |
| M-02 | TestClient 작업 관리자에서 강제 종료 (`End Process`) | 11초 이내 서버 로그에 "IdleTimeout detected. Session Id : N, IdleMs ≈ 10000" 출현, 세션 제거 |
| M-03 | TestClient 연결만 유지 (빈 연결, 송신 없음) | 11초 이내 idle disconnect (현재 설계상 의도된 동작) |

### 8.6 Seed Data / 테스트 인프라 요구사항

- `LibCommons::Logger::Create` 를 TEST_MODULE_INITIALIZE 에서 초기화 (기존 TimerQueueTests 패턴 재사용)
- `LibCommons::TimerQueue::GetInstance().Shutdown` 을 TEST_MODULE_CLEANUP 에서 Logger 보다 먼저 호출

---

## 9. Clean Architecture

### 9.1 Layer Structure

| Layer | Location | Responsibility |
|-------|----------|----------------|
| **Public Interface** | `LibNetworks/IIdleAware.ixx`, `SessionIdleChecker.ixx` | 인터페이스 + Config + API |
| **Implementation** | `LibNetworks/SessionIdleChecker.cpp` | TimerQueue 연동, 예외 처리 |
| **Existing Abstraction** | `LibNetworks/IOSession.{ixx,cpp}` | `IIdleAware` 구현 (신규 베이스 상속) |
| **Infrastructure** | `LibCommons::TimerQueue`, `LibCommons::Container`, `LibCommons::Logger` | 이미 존재 |
| **Consumer** | `FastPortServer/IOCPServiceMode.cpp` | 조립 + 생명주기 관리 |

### 9.2 Module Dependency Rules

```
 FastPortServer ──> networks.sessions.idle_checker
                 │
                 └─> networks.sessions.inbound_session (기존)
                        │
                        └─> networks.sessions.io_session (확장)
                               │
                               └─> networks.sessions.iidle_aware (신규)
                                      │
                                      └─> networks.sessions.inetwork_session (DisconnectReason 추가)

 networks.sessions.idle_checker ──> networks.sessions.iidle_aware
                                 └─> commons.timer_queue
                                 └─> commons.logger
```

### 9.3 This Feature's Layer Assignment

| Component | Layer | Location |
|-----------|-------|----------|
| `DisconnectReason` enum | Public Interface | `INetworkSession.ixx` (import cycle 회피) |
| `IIdleAware` interface | Public Interface | `IIdleAware.ixx` (신규) |
| `IdleCheckerConfig` | Public Interface | `SessionIdleChecker.ixx` |
| `SessionIdleChecker` | Public Interface | `SessionIdleChecker.ixx` + `.cpp` |
| `IOSession::m_LastRecvTimeMs` | Implementation | 기존 `IOSession` 확장 |
| Container::ForEach/Snapshot | Infrastructure | 기존 `Container.ixx` 확장 |

---

## 10. Coding Convention Reference

(CLAUDE.md 지침 준수)

| Item | Convention |
|------|------------|
| Module name | `networks.sessions.iidle_aware`, `networks.sessions.idle_checker` |
| Class name | PascalCase (`SessionIdleChecker`, `IIdleAware`) |
| Member variable | `m_` + PascalCase |
| Logger category | `"IdleChecker"` (세션 측은 기존 `"IOSession"`) |
| GMF include | `#include <spdlog/spdlog.h>` 반드시 포함 (Logger 템플릿 ICE 회피) |
| Logging helpers | 선행 피처 패턴 재사용 가능 — `LogICInfo/Warning/Error` 헬퍼 |
| Time source | `std::chrono::steady_clock` + ms 단위 int64_t |
| 예외 정책 | public API throw 금지, 콜백 catch-all |

---

## 11. Implementation Guide

### 11.1 File Structure

```
LibCommons/
└── Container.ixx                (수정) — ForEach + Snapshot 추가

LibNetworks/
├── INetworkSession.ixx          (수정) — DisconnectReason enum 추가
├── IIdleAware.ixx               (신규) — IIdleAware interface
├── IOSession.ixx                (수정) — inherit IIdleAware, m_LastRecvTimeMs, RequestDisconnect(reason)
├── IOSession.cpp                (수정) — OnIOCompleted lastRecv 갱신, RequestDisconnect(reason) 구현
├── SessionIdleChecker.ixx       (신규) — Config + Checker 선언
├── SessionIdleChecker.cpp       (신규) — Start/Stop/OnTick 구현
└── LibNetworks.vcxproj(.filters) (수정) — 신규 4개 파일 등록

FastPortServer/
├── IOCPServiceMode.ixx          (수정) — m_IdleChecker 멤버
└── IOCPServiceMode.cpp          (수정) — OnStarted 에서 생성/Start, OnStopped/OnShutdown 정리

LibCommonsTests/
└── ContainerTests.cpp           (신규) — ForEach/Snapshot 테스트 5개

LibNetworksTests/
├── SessionIdleCheckerTests.cpp  (신규) — L1 I-01~I-08 (8개)
└── IOSessionIdleTests.cpp       (신규, 선택) — L2 lastRecv 통합 3개
```

### 11.2 Implementation Order

1. [ ] **M1 — DisconnectReason + IIdleAware**: `INetworkSession.ixx` 에 enum, `IIdleAware.ixx` 신규
2. [ ] **M2 — Container 확장**: `ForEach`, `Snapshot` + 테스트 C-01~C-05
3. [ ] **M3 — IOSession 확장**: 인터페이스 상속, `m_LastRecvTimeMs`, `RequestDisconnect(reason)`. 기존 호출자 영향 없음 확인
4. [ ] **M4 — IOSession 수신 경로**: `OnIOCompleted` 에서 lastRecv 갱신 (bytes > 0 만)
5. [ ] **M5 — SessionIdleChecker**: Config/Start/Stop/OnTick 구현. Logger 헬퍼 적용
6. [ ] **M6 — 단위 테스트**: Mock IIdleAware + I-01~I-08. TEST_MODULE_INITIALIZE/CLEANUP
7. [ ] **M7 — IOCPServiceMode 연동**: OnStarted/OnStopped 에서 checker 생성/정리
8. [ ] **M8 — L3 수동 시나리오**: TestClient 강제 종료로 감지 확인
9. [ ] **M9 — 로깅/메시지 정리**: 최종 로그 메시지 확정, 카테고리 확인

### 11.3 Session Guide

#### Module Map

| Module | Scope Key | Description | Estimated Turns |
|--------|-----------|-------------|:---------------:|
| Enum + Interface | `interface` | M1: DisconnectReason + IIdleAware.ixx | 5-7 |
| Container 확장 | `container` | M2: ForEach/Snapshot + 테스트 | 6-8 |
| IOSession 확장 | `iosession` | M3+M4: 상속 + lastRecv 갱신 + RequestDisconnect(reason) | 10-14 |
| IdleChecker | `checker` | M5+M6: 구현 + 단위 테스트 | 15-20 |
| 연동 + 수동 검증 | `integration` | M7+M8+M9: IOCPServiceMode + 시나리오 확인 + 로그 정리 | 10-15 |

#### Recommended Session Plan

| Session | Phase | Scope | Turns |
|---------|-------|-------|:-----:|
| Session 1 (완료) | Plan + Design | 전체 | ~25 |
| Session 2 | Do | `--scope interface,container` | 15-20 |
| Session 3 | Do | `--scope iosession` | 15-20 |
| Session 4 | Do | `--scope checker` | 20-25 |
| Session 5 | Do | `--scope integration` | 15-20 |
| Session 6 | Check + Report | 전체 | 20-25 |

한 번에 진행하고 싶다면 `/pdca do session-idle-timeout` 으로 전체 가능 (하지만 분할 권장).

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-04-20 | Initial draft (Option B — Clean: IIdleAware + SessionIdleChecker 확정) | AnYounggun |
