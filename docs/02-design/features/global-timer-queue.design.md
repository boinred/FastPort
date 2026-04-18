# global-timer-queue Design Document

> **Summary**: Windows `CreateThreadpoolTimer` 기반 재사용 타이머 유틸. Clean 아키텍처(PImpl + 상태머신 + TimerJob 어댑터 + ScopedTimer RAII) 로 구현.
>
> **Project**: FastPort
> **Author**: AnYounggun
> **Date**: 2026-04-17
> **Status**: Draft
> **Planning Doc**: [global-timer-queue.plan.md](../../01-plan/features/global-timer-queue.plan.md)

---

## Context Anchor

> Copied from Plan document.

| Key | Value |
|-----|-------|
| **WHY** | 타임아웃·재시도·주기 감시 수요 증가, ad-hoc 구현 난립 방지 |
| **WHO** | FastPort 엔진 개발자, 1차 소비자는 후속 `session-idle-timeout` |
| **RISK** | TP 콜백 겹침 시 경쟁, 취소 레이스, 콜백 내 셀프 취소 데드락 |
| **SUCCESS** | Schedule/Cancel 동작, 1만 타이머 CPU<1%, 메모리 릭 0 |
| **SCOPE** | Phase 1: 핵심 API + 테스트. Phase 2: session-idle-timeout에서 실사용 검증 |

---

## 1. Overview

### 1.1 Design Goals

1. **API 이중 경로 통합**: 람다와 `ITimerCommand`를 내부에서 단일 표현으로 정규화
2. **상태의 명시적 관리**: 타이머 생명주기를 상태머신으로 추적하여 레이스·중복 취소 방지
3. **수명 안전성**: RAII 기반 `ScopedTimer`로 호출자 실수 방어
4. **Windows API 격리**: PImpl로 `<Windows.h>`, `<threadpoolapiset.h>`가 공용 헤더에 새어나가지 않게
5. **계측 가능성**: 모든 상태 전이를 로깅/메트릭 지점으로 노출

### 1.2 Design Principles

- **Single Responsibility**: Queue(스케줄링) / Job(실행 단위) / Entry(상태 + OS 핸들) / Handle(소비자 API)
- **Interface Segregation**: `ITimerCommand`는 최소 2 메서드 (Execute, Name)
- **Encapsulation**: Windows 타입 일절 노출 안 함 (PImpl)
- **Fail-Fast**: 잘못된 사용(이미 Shutdown된 Queue에 Schedule 등) 시 로그 + false 반환, 예외 throw 금지
- **No Global State Surprise**: 싱글톤은 **명시적 opt-in** (`GetInstance()` 호출자만)

---

## 2. Architecture Options

### 2.0 Architecture Comparison

| Criteria | A: Minimal | B: Clean | C: Pragmatic |
|----------|:-:|:-:|:-:|
| **Approach** | 단일 클래스 + 단순 맵 | PImpl + 상태머신 + 샤딩 + ScopedTimer | 모듈+람다 통일+단일 mutex |
| **New Files** | 2 | 4 | 2 |
| **Complexity** | Low | High | Medium |
| **Maintainability** | Medium | **High** | High |
| **Effort** | Low | High | Medium |
| **Risk** | Medium (분기 많음) | Low (명시적) | Low (균형) |

**Selected**: **Option B (Clean)** — **Rationale**:
- 후속 피처(`session-idle-timeout`) 및 장래 RPC 타임아웃·재시도·쿨다운 등 여러 소비자가 예정되어 있어 **유지보수성** 우선
- 명시적 상태머신으로 취소·겹침 레이스를 논리적으로 증명 가능
- PImpl로 Windows SDK 헤더를 소비자에게서 격리 → 빌드 시간·심볼 오염 감소
- `ScopedTimer` RAII로 소비자 실수 방지 (특히 세션 파괴 시 자동 정리)

### 2.1 Component Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                     Public API (TimerQueue.ixx)                 │
│                                                                 │
│   export class TimerQueue                                       │
│     - ScheduleOnce(delay, move_only_function, name)             │
│     - ScheduleOnce(delay, unique_ptr<ITimerCommand>)            │
│     - SchedulePeriodic(interval, move_only_function, name)      │
│     - SchedulePeriodic(interval, unique_ptr<ITimerCommand>)     │
│     - Cancel(TimerId)                                           │
│     - Shutdown(bool waitForCallbacks)                           │
│     - GetInstance()   [opt-in singleton]                        │
│                                                                 │
│   export struct ITimerCommand                                   │
│     - virtual Execute()                                         │
│     - virtual Name()                                            │
│                                                                 │
│   export class ScopedTimer   [RAII handle]                      │
│     - ctor(TimerQueue&, TimerId)                                │
│     - ~ScopedTimer() → queue.Cancel(id)                         │
│     - Release() → TimerId (소유권 포기)                          │
└─────────────────────────────────────────────────────────────────┘
                                │
                                │ pimpl_ 소유
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│              Implementation (TimerQueue.cpp, hidden)            │
│                                                                 │
│   class TimerQueue::Impl                                        │
│     - ShardedMap<TimerId, unique_ptr<Entry>>                    │
│     - atomic<uint64_t> nextId                                   │
│     - atomic<State> queueState { Running, ShuttingDown, Dead }  │
│                                                                 │
│   struct Entry                                                  │
│     - PTP_TIMER handle                                          │
│     - TimerJob job  (통일된 실행 단위)                            │
│     - atomic<EntryState> state                                  │
│       { Scheduled → Running → Completed/Cancelled }             │
│     - bool isPeriodic                                           │
│     - Duration interval (periodic 전용)                          │
│                                                                 │
│   struct TimerJob   (람다/Command 통일 어댑터)                    │
│     - std::move_only_function<void()> invoke                    │
│     - std::string name                                          │
│                                                                 │
│   static void CALLBACK TpCallback(PTP_CALLBACK_INSTANCE,        │
│                                   void* context, PTP_TIMER)    │
│     → trampoline: Entry* 복구 → state 전이 → job.invoke()         │
└─────────────────────────────────────────────────────────────────┘
                                │
                                │ 사용
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│           Windows SDK (<threadpoolapiset.h>)                    │
│    CreateThreadpoolTimer / SetThreadpoolTimer /                 │
│    WaitForThreadpoolTimerCallbacks / CloseThreadpoolTimer       │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 Data Flow

#### Schedule 플로우

```
User code: tq.ScheduleOnce(500ms, lambda, "check")
   │
   ▼
[TimerQueue::ScheduleOnce]
   │ queueState == Running 확인
   │ TimerJob { invoke: lambda, name: "check" } 생성
   │ Entry { handle: null, job, state: Scheduled, isPeriodic: false } 생성
   │ TimerId id = nextId.fetch_add(1)
   │ shardedMap.Insert(id, Entry)
   ▼
[Windows API]
   │ handle = CreateThreadpoolTimer(TpCallback, &entry, nullptr)
   │ SetThreadpoolTimer(handle, dueTime(-500ms relative), 0, 0)
   │
   ▼
  return id
```

#### Callback 플로우 (one-shot)

```
[TP Pool Thread]
   │ TpCallback(inst, context=Entry*, handle)
   │
   ▼
[state 전이: Scheduled → Running (CAS)]
   │ 실패 시 이미 Cancelled → 즉시 return
   │
   ▼
[job.invoke() 실행]  ← 호출자 thread-safety 책임 (Q3-a)
   │ 예외 catch → Logger.LogError
   │
   ▼
[state 전이: Running → Completed]
   │
   ▼
[isPeriodic == false 이므로]
   │ shardedMap.Remove(id)
   │ CloseThreadpoolTimer(handle)  (self-close 허용, MSDN OK)
```

#### Cancel 플로우

```
User code: tq.Cancel(id)
   │
   ▼
[Entry* 획득, shardedMap에서 조회 (아직 제거 안 함)]
   │ 없으면 return false
   │
   ▼
[state 전이 시도: Scheduled → Cancelled (CAS)]
   │  - 성공: 아직 콜백 시작 전 → Fast path
   │  - 실패 + state==Running: 콜백 진행 중 → Wait path
   │  - 실패 + state==Completed: 이미 끝남 → return true
   │  - 실패 + state==Cancelled: 중복 호출 → return false
   │
   ▼
[Wait path: 현재 실행 중 콜백 대기]
   │ WaitForThreadpoolTimerCallbacks(handle, TRUE)
   │  (셀프 취소 감지 시 FALSE 전달하여 데드락 방지)
   │
   ▼
[공통 후처리]
   │ SetThreadpoolTimer(handle, nullptr, 0, 0)  // 미래 발사 취소
   │ CloseThreadpoolTimer(handle)
   │ shardedMap.Remove(id)
   │ return true
```

#### Periodic 발사별 루프

```
tick 1: Scheduled → Running → invoke → Running → Scheduled (period 유지)
tick 2: Scheduled → Running → invoke → Running → Scheduled
...
Cancel 시: 현재 tick이 Running이면 Wait, 아니면 Fast path 동일
```

> **중요 (Q3-a 반영)**: TP가 동일 handle의 콜백을 **동시 디스패치**할 수 있다. 상태 전이 `Scheduled → Running` CAS는 **단일 엔트리 직렬화가 아니라 진입점 카운팅에 가깝게 설계**. 즉 두 번째 진입자도 Running을 볼 수 있고 그대로 `invoke` 실행 가능 (Q3-a 겹침 허용). Cancel 시 `WaitForThreadpoolTimerCallbacks(..., TRUE)`가 모든 진행 콜백 완료 보장.

### 2.3 Dependencies

| Component | Depends On | Purpose |
|-----------|-----------|---------|
| `TimerQueue` (public) | `ITimerCommand`, `TimerId`, `Duration` | API 노출 |
| `TimerQueue::Impl` | Windows threadpoolapiset, `LibCommons::Logger` | OS 타이머 + 로깅 |
| `Entry` | `TimerJob`, `PTP_TIMER` | 상태 + OS 핸들 보관 |
| `TimerJob` | `std::move_only_function` | 람다/Command 통일 표현 |
| `ScopedTimer` | `TimerQueue&`, `TimerId` | RAII 취소 |
| Consumers (e.g. session-idle-timeout) | `TimerQueue` (public만) | 스케줄링 |

---

## 3. Data Model

### 3.1 Internal Structures

```cpp
// TimerQueue.cpp (PImpl 내부, 외부 비공개)

enum class EntryState : std::uint8_t {
    Scheduled = 0,
    Running   = 1,
    Completed = 2,  // one-shot 실행 완료
    Cancelled = 3,
};

enum class QueueState : std::uint8_t {
    Running      = 0,
    ShuttingDown = 1,
    Dead         = 2,
};

struct TimerJob {
    std::move_only_function<void()> invoke;
    std::string name;
};

struct Entry {
    TimerId id;
    PTP_TIMER handle;                      // Windows 타이머 핸들
    TimerJob job;                          // 실행 단위 (통일)
    std::atomic<EntryState> state;         // 상태머신
    bool isPeriodic;
    Duration interval;                     // periodic 전용, one-shot은 0
    TimerQueue::Impl* owner;               // 콜백 trampoline에서 역참조
};
```

### 3.2 State Transitions

```
                 ┌──────────────┐
                 │  Scheduled   │  ← 신규 등록
                 └──────┬───────┘
                        │
          ┌─────────────┼──────────────┐
          │             │              │
          ▼             ▼              ▼
    [TP callback]  [Cancel 호출]  [Shutdown]
          │             │              │
          ▼             ▼              ▼
    ┌─────────┐   ┌───────────┐   ┌───────────┐
    │ Running │   │ Cancelled │   │ Cancelled │
    └────┬────┘   └───────────┘   └───────────┘
         │ invoke 완료
         │
    ┌────┴─────────────┐
    │                  │
    ▼ (one-shot)       ▼ (periodic)
┌──────────┐    ┌──────────────┐
│Completed │    │ Scheduled    │  ← 다음 tick 대기
└──────────┘    └──────────────┘

유효 전이:
  Scheduled → Running     (TP 콜백 시작 시 CAS 시도, 겹침 허용으로 실패해도 진행)
  Scheduled → Cancelled   (Cancel/Shutdown 에서 CAS 성공)
  Running   → Completed   (one-shot 콜백 끝)
  Running   → Scheduled   (periodic 콜백 끝, 다음 tick 대기)
  Running   → Cancelled   (콜백 실행 중 Cancel — Wait path에서 완료 후 표기)
  * Cancelled / Completed 는 종단 상태
```

### 3.3 Ownership & Sharded Map

```cpp
// TimerQueue.cpp 내부

class ShardedMap {
public:
    static constexpr std::size_t kShardCount = 16;  // 2^N 유지

    struct Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<TimerId, std::unique_ptr<Entry>> entries;
    };

    Shard& GetShard(TimerId id) noexcept {
        return shards_[id & (kShardCount - 1)];
    }

    void Insert(TimerId id, std::unique_ptr<Entry> entry);
    Entry* Find(TimerId id);                 // nullptr if not found
    std::unique_ptr<Entry> Extract(TimerId id);  // 맵에서 제거 후 소유권 이전

private:
    std::array<Shard, kShardCount> shards_;
};
```

- **샤딩 근거**: 동시 Schedule/Cancel 수가 많아질 때 단일 `mutex` 경합 회피. 16개면 대부분의 서버 워크로드에서 충분.
- `unique_ptr<Entry>` 소유로 수명 명확. TP 콜백에서는 `raw pointer` 만 참조 (맵이 소유한 동안 유효 보장).

---

## 4. API Specification

### 4.1 Public Header (`LibCommons/TimerQueue.ixx`)

```cpp
module;
#include <Windows.h>
#include <threadpoolapiset.h>
export module commons.timer_queue;

import std;

namespace LibCommons {

export using TimerId  = std::uint64_t;
export using Duration = std::chrono::milliseconds;

export constexpr TimerId kInvalidTimerId = 0;

// 커맨드 인터페이스 (메타데이터/정책 필요 시)
export struct ITimerCommand {
    virtual ~ITimerCommand() = default;
    virtual void Execute() = 0;
    virtual std::string_view Name() const noexcept = 0;
};

// Forward-declare (ScopedTimer가 참조)
export class TimerQueue;

// RAII 핸들 — 소멸 시 자동 Cancel
export class ScopedTimer {
public:
    ScopedTimer() noexcept = default;
    ScopedTimer(TimerQueue& queue, TimerId id) noexcept;
    ~ScopedTimer();

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

    ScopedTimer(ScopedTimer&& other) noexcept;
    ScopedTimer& operator=(ScopedTimer&& other) noexcept;

    // 취소 책임 포기. 반환된 id는 호출자가 Cancel 직접 관리
    TimerId Release() noexcept;

    TimerId Get() const noexcept { return m_Id; }
    bool IsValid() const noexcept { return m_Id != kInvalidTimerId; }

private:
    TimerQueue* m_pQueue = nullptr;
    TimerId     m_Id     = kInvalidTimerId;
};

export class TimerQueue {
public:
    TimerQueue();
    ~TimerQueue();

    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    // === 람다 경로 ===
    // Thread-safety: 호출자는 task가 여러 스레드에서 동시 실행될 수 있음을 고려해야 함 (Q3-a)
    TimerId ScheduleOnce(Duration delay,
                         std::move_only_function<void()> task,
                         std::string_view name = "anonymous");

    TimerId SchedulePeriodic(Duration interval,
                             std::move_only_function<void()> task,
                             std::string_view name = "anonymous");

    // === 커맨드 경로 ===
    TimerId ScheduleOnce(Duration delay, std::unique_ptr<ITimerCommand> cmd);
    TimerId SchedulePeriodic(Duration interval, std::unique_ptr<ITimerCommand> cmd);

    // 취소: true = 실제 취소 성공 or 이미 완료, false = id 없음/중복 호출
    bool Cancel(TimerId id);

    // 전체 종료. waitForCallbacks=true 면 모든 진행 중 콜백 대기
    void Shutdown(bool waitForCallbacks = true);

    // Opt-in 싱글톤
    static TimerQueue& GetInstance();

private:
    struct Impl;
    std::unique_ptr<Impl> m_pImpl;
};

} // namespace LibCommons
```

### 4.2 API Contract Details

| Method | Return | Side Effect | Errors |
|--------|--------|-------------|--------|
| `ScheduleOnce(...)` | `TimerId > 0` 또는 `kInvalidTimerId` | OS 타이머 생성, Entry 맵에 삽입 | QueueState != Running → `kInvalidTimerId` + warn log |
| `SchedulePeriodic(...)` | `TimerId > 0` 또는 `kInvalidTimerId` | 동일 + periodic 주기 설정 | 동일 |
| `Cancel(id)` | `bool` | OS 타이머 취소 + 대기(Wait path 시) + 맵 제거 | 없음 (오류 없이 false 반환) |
| `Shutdown(wait)` | `void` | QueueState=ShuttingDown → Dead, 모든 Entry Cancel, 옵션에 따라 대기 | 재호출은 무시 (idempotent) |
| `GetInstance()` | `TimerQueue&` | 첫 호출 시 정적 인스턴스 생성 | 없음 (Meyers 싱글톤) |

### 4.3 ScopedTimer 사용 예

```cpp
// 소비자 코드 — 세션 파괴 시 자동 취소
class Session {
    LibCommons::ScopedTimer m_IdleTimer;
public:
    void Start() {
        auto& tq = LibCommons::TimerQueue::GetInstance();
        auto id = tq.SchedulePeriodic(1s, [this]{ CheckIdle(); }, "Session::idle");
        m_IdleTimer = LibCommons::ScopedTimer(tq, id);
    }
    // ~Session(): m_IdleTimer 소멸 → 자동 Cancel, 진행 중 콜백 대기
};
```

### 4.4 Self-Cancel 가이드

```cpp
// 콜백 내부에서 자기 자신을 취소할 때
auto id = tq.ScheduleOnce(100ms, [&tq, idPtr = ...]{
    if (조건)
        tq.Cancel(*idPtr);  // ← 셀프 취소: 내부에서 Wait 생략 (데드락 방지)
}, "selfCancelExample");
```

내부 구현은 `WaitForThreadpoolTimerCallbacks(..., TRUE)`가 아닌 `FALSE` 를 전달하여 자기 자신을 기다리는 데드락 방지. 호출 중 취소는 "표식만" 하고 실제 cleanup은 콜백 리턴 후에 완료.

---

## 5. UI/UX Design

> **해당 없음**. 라이브러리 피처라 UI 없음.

---

## 6. Error Handling

### 6.1 Error Scenarios

| # | 상황 | 동작 | Logger |
|---|------|------|--------|
| 1 | `Schedule` 호출 시 QueueState != Running | `kInvalidTimerId` 반환 | Warn: "TimerQueue", "Schedule after Shutdown: name={}" |
| 2 | `CreateThreadpoolTimer` 실패 | `kInvalidTimerId` 반환, Entry 롤백 | Error: "TimerQueue", "CreateThreadpoolTimer failed. GLE={}" |
| 3 | `Cancel(invalidId)` | `false` 반환 | (로깅 없음 — 정상 경로) |
| 4 | 콜백 내부 예외 throw | catch 후 삼켜서 서버 보호 | Error: "TimerQueue", "Callback '{name}' threw: {what}" |
| 5 | `Shutdown` 중복 호출 | idempotent, 두 번째 이후는 noop | Info: "TimerQueue", "Shutdown already invoked" |
| 6 | 콜백 내 셀프 Cancel (데드락 가능) | Wait 생략 | Debug: "TimerQueue", "Self-cancel detected, skip wait" |

### 6.2 Error Format

예외 throw 없음. 모든 오류는 `bool` / `kInvalidTimerId` 반환 + `LibCommons::Logger` 로그.

### 6.3 Callback Exception Policy

```cpp
// TP 콜백 trampoline 내부
static void CALLBACK TpCallback(PTP_CALLBACK_INSTANCE, void* context, PTP_TIMER) {
    auto* entry = static_cast<Entry*>(context);
    try {
        entry->job.invoke();
    } catch (const std::exception& e) {
        LibCommons::Logger::GetInstance().LogError(
            "TimerQueue", "Callback '{}' threw: {}", entry->job.name, e.what());
    } catch (...) {
        LibCommons::Logger::GetInstance().LogError(
            "TimerQueue", "Callback '{}' threw unknown", entry->job.name);
    }
}
```

서버 전체가 타이머 콜백 예외로 죽지 않도록 **반드시 catch-all 사용**.

---

## 7. Security Considerations

라이브러리 피처이며 외부 입력을 받지 않음. 다만:

- [ ] `std::move_only_function` 캡처는 수명이 Entry에 묶임 — 콜백에서 참조하는 객체는 **Entry보다 오래 살아야** 함을 헤더 주석에 명시
- [ ] `ITimerCommand` 포인터는 `unique_ptr`로 소유 이전하여 dangling 방지
- [ ] `Shutdown` 이후 Schedule 시도 차단 → 종료 경로에서 타이머 누수 원천 차단
- [ ] 콜백 예외 전부 catch → 악의적/버그 콜백이 서버 프로세스 다운시키지 못하게

---

## 8. Test Plan

> C++ 라이브러리이므로 L1/L2/L3를 아래와 같이 재해석:
> - **L1 (Unit)**: 개별 API 호출 검증
> - **L2 (Integration)**: 상태 전이·타이밍 검증
> - **L3 (Stress)**: 동시성·부하·메모리 릭

### 8.1 Test Scope

| Type | Target | Tool | Phase |
|------|--------|------|-------|
| L1: Unit | Schedule/Cancel/Shutdown 기본 동작 | `LibCommonsTests` (자체 assert 매크로) | Do |
| L2: Integration | 상태 전이, 타이밍, periodic 발사 횟수, ScopedTimer | 동일 | Do |
| L3: Stress | 동시 등록/취소, 수명 관리, 릭 체크 | 동일 + `_CrtDumpMemoryLeaks` | Do |

### 8.2 L1 — Unit Test Scenarios

| # | Name | Description | Expected |
|---|------|-------------|----------|
| U-01 | `ScheduleOnce_ValidId` | 한 번 스케줄 후 반환 id 확인 | `id != kInvalidTimerId` |
| U-02 | `ScheduleOnce_RunsOnce` | delay 후 콜백 1회 실행 | 카운터 == 1, 이후 추가 실행 없음 |
| U-03 | `SchedulePeriodic_RunsN` | 100ms 주기, 500ms 대기 | 카운터 ≈ 5 (±1) |
| U-04 | `Cancel_PendingTimer` | 미실행 타이머 취소 | Cancel=true, 콜백 카운터=0 |
| U-05 | `Cancel_AlreadyCompleted` | 완료된 one-shot id 취소 | Cancel=true (또는 false, 둘 중 규약 문서화) |
| U-06 | `Cancel_InvalidId` | 없는 id 취소 | Cancel=false |
| U-07 | `Command_Execute` | `ITimerCommand` 경로 실행 | `cmd.Execute()` 호출됨 |
| U-08 | `Command_NameLogged` | 커맨드 `Name()` 로그에 기록 | Logger 출력에 이름 포함 |
| U-09 | `Shutdown_BlocksNewSchedule` | Shutdown 후 Schedule | 반환 `kInvalidTimerId`, warn 로그 |
| U-10 | `Shutdown_Idempotent` | Shutdown 두 번 호출 | 두 번째는 noop |

### 8.3 L2 — Integration Test Scenarios

| # | Name | Description | Expected |
|---|------|-------------|----------|
| I-01 | `StateTransition_Scheduled_Running_Completed` | one-shot 발사 후 상태 확인 | 내부 상태(테스트 전용 접근) == Completed |
| I-02 | `StateTransition_Cancel_Running` | 콜백 실행 중(sleep) Cancel | Wait 발생, Cancel 완료 후 콜백 확인 |
| I-03 | `Periodic_CancelDuringTick` | periodic 실행 중 Cancel | 현재 tick 완료, 이후 발사 없음 |
| I-04 | `ScopedTimer_AutoCancel` | ScopedTimer 소멸 시 자동 Cancel | 콜백 실행 없음 |
| I-05 | `ScopedTimer_Release` | Release() 후 소멸 | Cancel 호출 안 됨, 콜백 실행됨 |
| I-06 | `Timing_Within30ms` | 100ms delay 설정 | 실제 실행 지연 70~130ms (Windows 해상도 고려) |
| I-07 | `Callback_ExceptionSwallowed` | 콜백에서 `throw` | 서버 안 죽고 error 로그 1회 |
| I-08 | `Destructor_CancelsAll` | TimerQueue 소멸 시 활성 타이머 정리 | 릭 없음, 콜백 깨끗히 대기 후 종료 |

### 8.4 L3 — Stress Test Scenarios

| # | Name | Description | Expected |
|---|------|-------------|----------|
| S-01 | `Concurrent_Schedule_100threads_1000each` | 100 스레드 × 1000 Schedule | 크래시 없음, 모두 유효 id |
| S-02 | `Concurrent_ScheduleAndCancel` | 스케줄 중 동시 Cancel | 크래시 없음, 일부 Cancel true/일부 false |
| S-03 | `Memory_Leak_10k_Schedule_Cancel` | 1만 Schedule → 1만 Cancel | 프로세스 메모리 증가 < 1MB, `_CrtDumpMemoryLeaks` 0 |
| S-04 | `Overlap_PeriodicFastInterval` | 주기 < 콜백 실행시간인 케이스 | 겹침 허용 확인 (Q3-a), 서버 안정 |
| S-05 | `Shutdown_UnderLoad` | 1만 활성 타이머 상태에서 Shutdown | 대기 종료, 릭 없음 |

### 8.5 Test Seed/Setup

- `LibCommonsTests.vcxproj` 에 `TimerQueueTests.cpp` 등록
- Logger 를 테스트 중 리디렉션 (output capture) — 기존 테스트 패턴 따라감
- 테스트 격리: 각 테스트는 새 `TimerQueue` 인스턴스 생성 (싱글톤 경로는 별도 1개 테스트만)

---

## 9. Clean Architecture

### 9.1 Layer Structure (C++ Native 프로젝트 적용)

| Layer | Responsibility | Location |
|-------|---------------|----------|
| **Public Interface** | 모듈 export 심볼, 소비자 계약 | `LibCommons/TimerQueue.ixx` |
| **Implementation (Hidden)** | OS API 호출, 상태머신, 락 | `LibCommons/TimerQueue.cpp` (`Impl`, `Entry`, `TimerJob`, `ShardedMap`) |
| **Adapter** | 람다/Command 통일, RAII 핸들 | `TimerJob` (내부), `ScopedTimer` (공개) |
| **Infrastructure** | Windows threadpoolapiset, Logger | `<threadpoolapiset.h>`, `LibCommons::Logger` |

### 9.2 Dependency Rules

```
┌──────────────────────────────────────────────────────────────┐
│               Consumers (LibNetworks, 후속 피처)               │
│                          │                                   │
│                          │ import commons.timer_queue        │
│                          ▼                                   │
│               ┌──────────────────────┐                       │
│               │ TimerQueue.ixx (API) │                       │
│               └──────────┬───────────┘                       │
│                          │ PImpl                             │
│                          ▼                                   │
│               ┌──────────────────────┐                       │
│               │ TimerQueue.cpp (Impl)│                       │
│               └──────────┬───────────┘                       │
│                          │                                   │
│              ┌───────────┴───────────┐                       │
│              ▼                       ▼                       │
│        Windows SDK           LibCommons::Logger              │
│        (threadpoolapiset)    (import commons.logger)         │
└──────────────────────────────────────────────────────────────┘

Rule: Consumers는 Impl 내부 타입(Entry, TimerJob, PTP_TIMER) 에 접근 불가.
      모듈 export가 이를 강제.
```

### 9.3 Import/Include Rules

| From | Can Import/Include | Cannot |
|------|--------------------|--------|
| `TimerQueue.ixx` (global module fragment) | `<Windows.h>`, `<threadpoolapiset.h>`, `<chrono>`, `<functional>`, `<memory>`, `<string_view>`, `<cstdint>` | 상위 프로젝트 심볼 |
| `TimerQueue.ixx` (after `export module`) | `import std;` | — |
| `TimerQueue.cpp` | 위 + `import commons.logger;`, `<unordered_map>`, `<shared_mutex>`, `<array>`, `<atomic>` | — |
| Consumers | `import commons.timer_queue;` | `TimerQueue.cpp` 내부 타입 |

### 9.4 This Feature's Layer Assignment

| Component | Layer | Location |
|-----------|-------|----------|
| `TimerQueue` (class) | Public Interface | `TimerQueue.ixx` |
| `ITimerCommand` | Public Interface | `TimerQueue.ixx` |
| `ScopedTimer` | Public Interface (Adapter) | `TimerQueue.ixx` |
| `TimerQueue::Impl` | Implementation (Hidden) | `TimerQueue.cpp` |
| `Entry`, `TimerJob`, `ShardedMap`, `EntryState`, `QueueState` | Implementation (Hidden) | `TimerQueue.cpp` anonymous namespace |
| `TpCallback` (static) | Implementation (Hidden) | `TimerQueue.cpp` anonymous namespace |

---

## 10. Coding Convention Reference

### 10.1 Naming Conventions (기존 프로젝트 준수)

| Target | Rule | Example |
|--------|------|---------|
| Class / Struct | PascalCase | `TimerQueue`, `ScopedTimer`, `Entry` |
| Public methods | PascalCase | `ScheduleOnce`, `Cancel`, `Shutdown` |
| Private methods | PascalCase | `InsertEntry`, `RunCallback` |
| Free functions (internal) | PascalCase | `TpCallback` |
| Member variables | `m_` + PascalCase | `m_pImpl`, `m_Id`, `m_pQueue` |
| Static / const | `k` + PascalCase | `kInvalidTimerId`, `kShardCount` |
| Enum values | PascalCase | `EntryState::Scheduled` |
| Module name | snake_case | `commons.timer_queue` |

### 10.2 Include Order

```cpp
// 1. module fragment (.ixx 최상단)
module;

// 2. System / OS headers (GMF)
#include <Windows.h>
#include <threadpoolapiset.h>

// 3. Module declaration
export module commons.timer_queue;

// 4. Imports
import std;

// 5. (.cpp 전용) 추가 헤더 / import
import commons.logger;
```

### 10.3 Thread-Safety Documentation

모든 public 메서드 위에 thread-safety 주석 **필수**:

```cpp
// Thread-safety: Concurrent calls from multiple threads are safe.
// task may be invoked concurrently on TP pool threads — caller must ensure thread-safety.
TimerId ScheduleOnce(Duration delay, std::move_only_function<void()> task, std::string_view name);
```

### 10.4 Error Handling Convention

| Situation | Convention |
|-----------|------------|
| 실패 (사용자 잘못) | `bool false` / `kInvalidTimerId` 반환 + `Logger::LogWarning` |
| OS API 실패 | 동일 반환 + `Logger::LogError` + `::WSAGetLastError()` 또는 `::GetLastError()` 포함 |
| 콜백 예외 | 전부 catch, 로그만 |
| 예외 throw | **금지** (이 모듈의 public API 누구도 throw 하지 않음) |

### 10.5 This Feature's Conventions

| Item | Convention Applied |
|------|-------------------|
| 모듈 구성 | C++20 module `commons.timer_queue`, PImpl 패턴 |
| 메모리 관리 | `std::unique_ptr<Entry>` 맵 소유, raw pointer는 콜백 context에만 |
| 동기화 | `std::shared_mutex` 샤딩 (16), `std::atomic` 상태 전이 |
| 로깅 | `LibCommons::Logger::GetInstance()` 싱글톤 사용 |
| 예외 정책 | noexcept where possible, callback 예외는 catch-all |

---

## 11. Implementation Guide

### 11.1 File Structure

```
LibCommons/
├── TimerQueue.ixx           (신규) — 모듈 인터페이스: TimerQueue, ITimerCommand, ScopedTimer
├── TimerQueue.cpp           (신규) — Impl, Entry, TimerJob, ShardedMap, TpCallback
├── LibCommons.vcxproj       (수정) — 소스 2개 추가
└── LibCommons.vcxproj.filters (수정)

LibCommonsTests/
├── TimerQueueTests.cpp      (신규) — L1/L2/L3 테스트
├── LibCommonsTests.vcxproj  (수정)
└── LibCommonsTests.vcxproj.filters (수정)
```

### 11.2 Implementation Order

1. [ ] **M1 — 모듈 골격**: `TimerQueue.ixx` public API 선언만 (컴파일 통과, 구현 stub)
2. [ ] **M2 — Impl 기본**: `TimerQueue::Impl`, `Entry`, `TimerJob`, `ShardedMap` 정의
3. [ ] **M3 — Schedule/Callback**: `ScheduleOnce` + `TpCallback` 최소 경로 동작 (one-shot)
4. [ ] **M4 — 상태머신 + Cancel**: `EntryState` CAS 전이, `Cancel` 플로우 (Fast/Wait path)
5. [ ] **M5 — Periodic**: `SchedulePeriodic`, Running→Scheduled 전이 루프
6. [ ] **M6 — Command 경로**: `ITimerCommand` 오버로드 (람다로 감싸서 TimerJob 생성)
7. [ ] **M7 — ScopedTimer**: RAII 핸들 클래스 + move semantics
8. [ ] **M8 — Shutdown + Singleton**: `Shutdown`, `GetInstance` 구현 + QueueState 전이
9. [ ] **M9 — 로깅 + 예외 처리**: 오류 경로 로거 연결, 콜백 catch-all
10. [ ] **M10 — 테스트**: L1 → L2 → L3 순서로 작성 및 통과

### 11.3 Session Guide

#### Module Map

| Module | Scope Key | Description | Estimated Turns |
|--------|-----------|-------------|:---------------:|
| 골격 + Impl 기본 | `core` | M1+M2: 모듈 선언, Impl/Entry/ShardedMap 스켈레톤 | 8-12 |
| Schedule+Callback | `schedule` | M3+M6: one-shot + Command 경로 통합, TpCallback trampoline | 10-15 |
| 상태머신+Cancel | `cancel` | M4+M5: EntryState CAS, Cancel Fast/Wait, Periodic 루프 | 12-18 |
| 수명관리+종료 | `lifecycle` | M7+M8+M9: ScopedTimer, Shutdown, Singleton, 로깅/예외 | 10-15 |
| 테스트 | `tests` | M10: L1 → L2 → L3 테스트 작성, 프로젝트 파일 갱신 | 15-25 |

#### Recommended Session Plan

| Session | Phase | Scope | Turns |
|---------|-------|-------|:-----:|
| Session 1 (완료) | PM + Plan + Design | — | ~25 |
| Session 2 | Do | `--scope core,schedule` | 20-30 |
| Session 3 | Do | `--scope cancel,lifecycle` | 25-35 |
| Session 4 | Do | `--scope tests` | 15-25 |
| Session 5 | Check + Report | 전체 | 20-30 |

> `--scope` 구분은 세션 분할 가이드. 한 번에 진행하고 싶다면 `/pdca do global-timer-queue` 로 전체 진행 가능.

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-04-17 | Initial draft (Option B — Clean selected) | AnYounggun |
