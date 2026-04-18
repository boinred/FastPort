# global-timer-queue Planning Document

> **Summary**: Windows `CreateThreadpoolTimer` 기반 재사용 가능한 글로벌 TimerQueue 유틸리티. C++20 모듈 파사드, move-only functor + ITimerCommand 하이브리드 API.
>
> **Project**: FastPort
> **Author**: AnYounggun
> **Date**: 2026-04-17
> **Status**: Draft

---

## Executive Summary

| Perspective | Content |
|-------------|---------|
| **Problem** | 서버 전반에서 "N초 후 또는 N초마다 특정 작업 실행"이 필요한 지점(세션 idle 감지, RPC 타임아웃, 재시도 스케줄, 쿨다운)이 늘어나는데 공용 타이머 인프라가 없음 |
| **Solution** | `LibCommons::TimerQueue` — Windows 모던 스레드풀(`CreateThreadpoolTimer`)을 C++20 모듈로 감싼 재사용 유틸. 람다/`ITimerCommand` 하이브리드 API, RAII 수명 관리 |
| **Function/UX Effect** | `tq.ScheduleOnce(500ms, []{...}, "name")` / `tq.SchedulePeriodic(1s, cmd)` 한 줄로 타이머 등록, `Cancel(id)`로 즉시 취소. 콜백은 TP 풀 스레드에서 바로 실행 |
| **Core Value** | 게임 서버 전반의 시간 기반 작업 표준화, 세션 idle timeout·RPC timeout 등 상위 피처의 기반. Windows 커널 최적화 스레드풀 활용으로 수만 타이머까지 확장 가능 |

---

## Context Anchor

| Key | Value |
|-----|-------|
| **WHY** | 시간 기반 작업(타임아웃·재시도·주기 감시)이 여러 모듈에서 필요해지는데 공용 인프라 부재 → 각자 ad-hoc 구현 위험 |
| **WHO** | FastPort 엔진/서버 내부 개발자(본인), 후속 피처 `session-idle-timeout`의 1차 소비자 |
| **RISK** | TP 콜백 겹침 시 데이터 경쟁, 취소 레이스(`WaitForThreadpoolTimerCallbacks`), 콜백 내부에서 TimerQueue 재진입(self-cancel) 시 데드락 |
| **SUCCESS** | ScheduleOnce/Periodic/Cancel 동작, 1만 타이머 등록 시 CPU < 1%, 메모리 릭 0, 취소 레이스 없음 |
| **SCOPE** | Phase 1: 핵심 API(ScheduleOnce/Periodic/Cancel/Shutdown) + 하이브리드 오버로드 + 단위 테스트. Phase 2(후속): 세션 idle timeout에서 실사용 검증 |

---

## 1. Overview

### 1.1 Purpose

Windows 환경에서 지연·주기 실행이 필요한 작업을 등록하고 관리하는 **재사용 가능한 글로벌 타이머 유틸리티**를 제공한다. 후속 피처인 `session-idle-timeout`의 기반이며, RPC 타임아웃/재시도/쿨다운 등 장래 타이머 수요를 모두 수용한다.

### 1.2 Background

- FastPort는 Windows 전용 고성능 네트워크 엔진(IOCP + RIO)
- 현재 타이머 없음. 세션 단절 감지는 TCP Keep-Alive(30s)에 의존 → 비정상 단절 시 유령 세션 리스크 (PRD `session-idle-timeout.prd.md` 참조)
- `LibCommons`에 `ThreadPool`은 있지만 **시간 기반 스케줄링**은 없음
- 공용 타이머 없이 각 모듈에서 개별 구현하면 버그·경쟁 조건 재발 위험 (이미 RIO 안정성 수정 5건 이력 — 커밋 `72d53cd`)

### 1.3 Related Documents

- PRD (후속 피처): `docs/00-pm/session-idle-timeout.prd.md`
- 참고 구현 스타일: `LibCommons/ThreadPool.ixx` (모듈 선언 + stop_token 패턴)
- MSDN: [CreateThreadpoolTimer](https://learn.microsoft.com/en-us/windows/win32/api/threadpoolapiset/nf-threadpoolapiset-createthreadpooltimer)

---

## 2. Scope

### 2.1 In Scope

- [ ] `LibCommons/TimerQueue.ixx` — C++20 모듈 인터페이스 (`export module commons.timer_queue`)
- [ ] `LibCommons/TimerQueue.cpp` — 모듈 구현
- [ ] 하이브리드 Task API:
  - [ ] 람다용 오버로드: `ScheduleOnce(delay, std::move_only_function<void()>, name)` / `SchedulePeriodic(...)`
  - [ ] 커맨드용 오버로드: `ScheduleOnce(delay, std::unique_ptr<ITimerCommand>)` / `SchedulePeriodic(...)`
- [ ] `ITimerCommand` 순수 가상 인터페이스 (`Execute()`, `Name()`)
- [ ] `TimerId` (64-bit) 기반 취소 핸들 + `Cancel(TimerId)`
- [ ] RAII 기반 수명 관리 (`~TimerQueue()`에서 모든 활성 타이머 정리)
- [ ] `Shutdown()` 명시 호출 지원 (진행 중 콜백 대기 옵션 포함)
- [ ] 싱글톤 옵션 (선택): `TimerQueue::GetInstance()` — 프로젝트에 기존 `SingleTon.ixx` 패턴 재사용
- [ ] 단위 테스트 (`LibCommonsTests/TimerQueueTests.cpp`): 기본 동작, 취소, 주기, 동시성, 수명 관리
- [ ] Logger 연동: 타이머 등록/취소/예외 로그 (`LibCommons::Logger`)

### 2.2 Out of Scope

- **콜백 스레드 제어** (IOCP 포스트 등) — 이번 피처는 TP 풀 스레드에서 **그대로 실행** (Q5-a 결정)
- **겹침 방지** — 호출자 thread-safety 책임 (Q3-a 결정, 최대 처리량 우선)
- **우선순위 큐 / fairness** — YAGNI
- **mock clock / 시간 주입 추상화** — 유닛 테스트는 `sleep_for` 기반 통합 테스트로 대체 (추후 `IClock` 추가 가능)
- **세션 idle timeout 로직** — 후속 피처 `session-idle-timeout`에서 이 TimerQueue 소비
- **크로스 플랫폼 지원** — Windows 전용 (프로젝트 전체 방향)
- **동적 주기 변경** — 등록 시 정해진 주기 유지. 필요하면 Cancel + 재등록

---

## 3. Requirements

### 3.1 Functional Requirements

| ID | Requirement | Priority | Status |
|----|-------------|----------|--------|
| FR-01 | `ScheduleOnce(delay, task, name)` — N ms 후 1회 실행, `TimerId` 반환 | High | Pending |
| FR-02 | `SchedulePeriodic(interval, task, name)` — N ms마다 반복 실행, `TimerId` 반환 | High | Pending |
| FR-03 | `Cancel(TimerId)` — 미실행 콜백 취소 + 실행 중 콜백 완료 대기, 성공 여부 반환 | High | Pending |
| FR-04 | 람다 오버로드: `std::move_only_function<void()>` 수용 (move-only capture 가능) | High | Pending |
| FR-05 | 커맨드 오버로드: `std::unique_ptr<ITimerCommand>` 수용 | High | Pending |
| FR-06 | `ITimerCommand::Name()` 로 로깅/추적 용이 | Medium | Pending |
| FR-07 | TimerQueue 소멸 시 모든 활성 타이머 자동 정리 (RAII) | High | Pending |
| FR-08 | `Shutdown(bool waitForCallbacks)` — 명시적 종료 지원 | Medium | Pending |
| FR-09 | Logger 연동 (등록/취소/콜백 예외) | Medium | Pending |
| FR-10 | 싱글톤 접근 (`GetInstance()`) — 프로젝트 전역 타이머 공유용 (선택) | Low | Pending |

### 3.2 Non-Functional Requirements

| Category | Criteria | Measurement Method |
|----------|----------|-------------------|
| **Performance — throughput** | 1만 타이머 등록/발사 시 CPU < 1% (idle 시) | `FastPortBenchmark` 스타일 측정, PerfMon |
| **Performance — latency** | 설정된 delay 기준 ±30ms 이내 (기본 Windows 타이머 해상도 고려) | 단위 테스트 `chrono` 측정 |
| **Memory safety** | 타이머 1만 개 등록 → 전부 취소 → 릭 0 | VLD / `_CrtDumpMemoryLeaks` |
| **Thread safety** | 동시 등록/취소 100 스레드 × 1000회 → 크래시·데드락 없음 | 스트레스 테스트 |
| **API ergonomics** | 호출부 한 줄(`tq.ScheduleOnce(...)`)로 등록 | 코드 리뷰 |
| **Build** | MSVC 2022, C++20 modules (`/std:c++20`), Release/Debug 양쪽 빌드 성공 | `FastPort.slnx` 빌드 |

---

## 4. Success Criteria

### 4.1 Definition of Done

- [ ] FR-01 ~ FR-09 구현 완료 (FR-10은 선택)
- [ ] `LibCommonsTests/TimerQueueTests.cpp` 작성 — 아래 케이스 포함:
  - [ ] ScheduleOnce 실행 시점 검증
  - [ ] SchedulePeriodic N회 실행 검증
  - [ ] Cancel 미실행 타이머
  - [ ] Cancel 실행 중 타이머 (완료 대기 확인)
  - [ ] TimerQueue 소멸 시 모든 타이머 자동 정리
  - [ ] 동시 등록/취소 스트레스 (100 스레드 × 1000회)
  - [ ] `ITimerCommand` 경로와 람다 경로 동등 동작
- [ ] 단위 테스트 전부 pass
- [ ] Debug/Release 빌드 성공 (Win64)
- [ ] 메모리 릭 확인 (`_CrtSetDbgFlag` 또는 VLD)
- [ ] Logger 출력 확인 (등록/취소/예외 경로)

### 4.2 Quality Criteria

- [ ] 단위 테스트 커버리지 핵심 API 100%
- [ ] MSVC warning level 4 기준 경고 0
- [ ] `clang-format` 적용 (프로젝트 규칙 준수)
- [ ] 공용 헤더 주석은 **최소한** — 호출부가 API 이름만으로 이해 가능하도록

---

## 5. Risks and Mitigation

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| **TP 콜백이 동일 타이머에 중첩 발사 → 공유 상태 경쟁** | High | Medium | Q3-a 결정에 따라 호출자 책임. 문서/헤더 주석에 **명시적 경고**. `ITimerCommand::Execute()`는 thread-safe 해야 함을 문서화 |
| **`Cancel()` vs 콜백 실행 레이스** | High | Medium | `SetThreadpoolTimer(handle, nullptr, 0, 0)` + `WaitForThreadpoolTimerCallbacks(handle, TRUE)` 시퀀스로 OS 가이드 준수. 취소 중 `Cancel()` 재호출 방지(`atomic<bool> cancelling`) |
| **콜백 내부에서 `Cancel(ownId)` 호출 시 데드락** | High | Medium | 콜백 내 셀프 취소 감지 로직 + `WaitForThreadpoolTimerCallbacks` 호출 생략 (`FALSE` 전달). 문서에 "콜백 내 셀프 취소는 fire-and-forget" 명시 |
| **TimerQueue 소멸 중 콜백이 접근한 멤버 해제** | Critical | Low | 모든 활성 타이머 핸들을 소유하고, 소멸자에서 **순서 보장**: 신규 스케줄 차단 → 모든 타이머 Cancel(wait) → 내부 맵 정리 |
| **`move_only_function` → C 콜백(`PTP_TIMER_CALLBACK`) 트램펄린 실수** | Medium | Medium | `Context*`를 엔트리 구조체로 힙 저장, `std::unique_ptr` 소유. 트램펄린에서 `static_cast<Entry*>(context)->Execute()` |
| **타이머 해상도 부족** (~15ms) | Low | High | `timeBeginPeriod(1)` 호출 **금지** (전역 영향). 대신 설정 기본값을 ≥50ms로 가이드 |
| **싱글톤 패턴과 생성/소멸 순서** | Medium | Low | Meyers' Singleton (기존 `SingleTon.ixx` 패턴) 사용. 정적 파괴 순서에 의존하는 소비자 없도록 주의 |
| **MSVC C++20 모듈 + Windows SDK `<threadpoolapiset.h>` 호환성** | Medium | Low | `module;` 블록에 global module fragment로 Windows 헤더 include, `import std;` 혼용 패턴은 `ThreadPool.ixx` 따라감 |
| **후속 피처(session-idle-timeout) API 요구 변경** | Medium | Medium | 순차 진행(Q4) 채택으로 완료 후 실사용 검증 → 필요 시 비파괴 확장(오버로드 추가) |

---

## 6. Impact Analysis

### 6.1 Changed Resources

| Resource | Type | Change Description |
|----------|------|--------------------|
| `LibCommons/TimerQueue.ixx` | New Module Interface | 신규 추가 |
| `LibCommons/TimerQueue.cpp` | New Module Impl | 신규 추가 |
| `LibCommons/LibCommons.vcxproj` | Project File | 신규 소스 파일 등록 |
| `LibCommons/LibCommons.vcxproj.filters` | Project Filter | 소스 그룹 등록 |
| `LibCommonsTests/TimerQueueTests.cpp` | New Test | 신규 추가 |
| `LibCommonsTests/LibCommonsTests.vcxproj(.filters)` | Project File | 신규 테스트 파일 등록 |

### 6.2 Current Consumers

**이번 피처는 신규 모듈이므로 기존 소비자 없음.** 단, 후속에서 다음이 소비 예정:

| Resource | Operation | Code Path | Impact |
|----------|-----------|-----------|--------|
| `LibCommons::TimerQueue` | Schedule | (후속) `session-idle-timeout` 의 세션 스캔 주기 틱 | New dependency |
| `LibCommons::TimerQueue` | Schedule | (장래) RPC 타임아웃, 재전송 타이머 | New dependency |

### 6.3 Verification

- [ ] 신규 모듈이므로 기존 빌드 대상(FastPortServer/Client/Benchmark/TestClient) 빌드 영향 없음 확인
- [ ] `LibCommons.vcxproj` 의존 프로젝트 전체 재빌드 시 성공
- [ ] 기존 `ThreadPool`, `Logger`와 심볼 충돌 없음
- [ ] 싱글톤 채택 시 `LibCommons` 정적 초기화 순서 이슈 없음

---

## 7. Architecture Considerations

### 7.1 Project Level Selection

| Level | Characteristics | Recommended For | Selected |
|-------|-----------------|-----------------|:--------:|
| **Starter** | Simple static sites | 웹 포트폴리오 | ☐ |
| **Dynamic** | Feature-based modules | SaaS MVP | ☐ |
| **Enterprise** | Strict layer separation, DI, native 고성능 시스템 | 고성능 게임/네트워크 엔진 | ☑ |

### 7.2 Key Architectural Decisions

> C++ 네이티브 라이브러리 프로젝트이므로 템플릿의 웹 스택 항목을 C++ 관련으로 치환.

| Decision | Options | Selected | Rationale |
|----------|---------|----------|-----------|
| Backend mechanism | (α-legacy) `CreateTimerQueueTimer` / (α-modern) `CreateThreadpoolTimer` / (β) custom min-heap | **α-modern (γ: C++ 파사드)** | Windows 전용 확정 + IOCP와 동일 TP 인프라 + 커널 최적화. Q2 결정 |
| Task representation | (A) 람다만 / (B) ITimerCommand만 / (C) 하이브리드 | **C (하이브리드)** | 일반 호출부는 람다로 간결, 정책(재시도·메타)이 필요하면 Command 상속. Q1 결정 |
| Callback thread | (a) TP pool 직접 / (b) IOCP 포스트 | **(a) TP pool 직접** | 성능 최대. 콜백 thread-safety는 호출자 책임. Q5 결정 |
| Periodic overlap policy | (a) 겹침 허용 / (b) 직렬화+drop / (c) 정책 선택 | **(a) 겹침 허용** | 최대 처리량, 최소 구현. 호출자 책임 명시. Q3 결정 |
| API 수명 모델 | raw handle / RAII 래퍼 / shared handle | **RAII + TimerId 토큰** | 내부 `PTP_TIMER`는 TimerQueue가 소유, 외부엔 64-bit `TimerId`만 노출 |
| Module system | Header-only `.h` / C++20 module (`.ixx`) | **C++20 module** | 프로젝트 표준(`ThreadPool.ixx`, `Logger.ixx` 등 전부 모듈) |
| Logger | `printf` / `LibCommons::Logger` | **LibCommons::Logger** | 프로젝트 표준 싱글톤 로거 사용 |
| Singleton | provided / 소비자가 소유 / both | **both (옵션 제공)** | 기본은 인스턴스 생성 자유, `GetInstance()` 정적 접근도 제공. 소비자 취향 |

### 7.3 Clean Architecture Approach

```
Selected Level: Enterprise (C++ native)

Module Layout:
┌─────────────────────────────────────────────────────┐
│ LibCommons (공용 유틸)                                  │
│   ├─ TimerQueue.ixx      ← 신규 (module commons.timer_queue) │
│   ├─ TimerQueue.cpp      ← 신규                           │
│   ├─ ThreadPool.ixx      (기존)                          │
│   ├─ Logger.ixx          (기존)                          │
│   └─ SingleTon.ixx       (기존, 재사용)                   │
│                                                     │
│ LibCommonsTests                                     │
│   └─ TimerQueueTests.cpp ← 신규 (Google Test 혹은 가벼운 assert 기반) │
└─────────────────────────────────────────────────────┘

Dependency Flow:
  (consumer)
  LibNetworks / FastPortServer / (장래) session-idle-timeout
           │ import
           ▼
     commons.timer_queue  ──►  Windows threadpoolapiset
                          └──►  LibCommons::Logger
```

### 7.4 API Sketch (Plan-level, 확정은 Design에서)

```cpp
// LibCommons/TimerQueue.ixx
module;
#include <Windows.h>
#include <threadpoolapiset.h>
export module commons.timer_queue;

import std;
import commons.logger;

namespace LibCommons {

export using TimerId  = std::uint64_t;
export using Duration = std::chrono::milliseconds;

export struct ITimerCommand {
    virtual ~ITimerCommand() = default;
    virtual void Execute() = 0;
    virtual std::string_view Name() const noexcept = 0;
};

export class TimerQueue {
public:
    TimerQueue();
    ~TimerQueue();

    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    // 람다 오버로드 (일반 사용)
    TimerId ScheduleOnce(Duration delay,
                         std::move_only_function<void()> task,
                         std::string_view name = "anonymous");
    TimerId SchedulePeriodic(Duration interval,
                             std::move_only_function<void()> task,
                             std::string_view name = "anonymous");

    // 커맨드 오버로드 (정책/메타 필요 시)
    TimerId ScheduleOnce(Duration delay,
                         std::unique_ptr<ITimerCommand> cmd);
    TimerId SchedulePeriodic(Duration interval,
                             std::unique_ptr<ITimerCommand> cmd);

    bool Cancel(TimerId id);
    void Shutdown(bool waitForCallbacks = true);

    static TimerQueue& GetInstance();  // 선택 (FR-10)

private:
    struct Entry;  // 내부 정의
    // ... PTP_TIMER 맵, mutex, atomic id 카운터 등
};

} // namespace LibCommons
```

---

## 8. Convention Prerequisites

### 8.1 Existing Project Conventions

- [x] `CLAUDE.md` 프로젝트 루트 존재 (스킬 라우팅 규칙만 포함, C++ 규약 명시적 문서 없음)
- [ ] `docs/01-plan/conventions.md` 부재
- [ ] `CONVENTIONS.md` 부재
- [ ] 통일된 `.clang-format` — 코드에서 기존 스타일 따라감 (파스칼케이스 클래스, m_접두 멤버)
- [x] MSVC 2022 기본 설정 사용 (`Application.props`, `Commons.props`)

### 8.2 Conventions to Define/Verify

| Category | Current State | To Define | Priority |
|----------|---------------|-----------|:--------:|
| **Naming** | 기존 코드: `PascalCaseClass`, `m_` 접두 멤버, `k` 접두 상수 | 기존 스타일 그대로 준수 | High |
| **Module naming** | `commons.logger`, `commons.thread_pool` 패턴 | `commons.timer_queue` 사용 | High |
| **Header guard** | 모듈 기반이라 불필요 | — | — |
| **Error handling** | `LibCommons::Logger` 로그 + `std::expected` 혼용? | 본 피처는 `bool` 반환 + 로그, 예외 throw 금지 | High |
| **Thread safety 문서화** | 전역 규약 없음 | 공용 API 위 `// Thread-safety: ...` 주석 필수 | Medium |

### 8.3 Environment Variables Needed

| Variable | Purpose | Scope | To Be Created |
|----------|---------|-------|:-------------:|
| (없음) | 이번 피처는 환경 변수 불필요 | — | ☐ |

### 8.4 Pipeline Integration

> 9-phase 개발 파이프라인은 웹/BaaS 대상이라 이 C++ 라이브러리 피처에는 해당 없음. PDCA만 적용.

---

## 9. Next Steps

1. [ ] Design 문서 작성 (`/pdca design global-timer-queue`)
   - 3가지 구현 옵션 중 선택 (이미 γ 확정 상태이므로 변형안 비교)
   - 내부 자료구조(Entry 구조체, 맵 타입), 락 전략, 취소 시퀀스 상세
2. [ ] Implementation (`/pdca do global-timer-queue`)
3. [ ] Gap analysis (`/pdca analyze global-timer-queue`)
4. [ ] 완료 후 **후속 피처** `/pdca plan session-idle-timeout` 순차 진행 (Q4)

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-04-17 | Initial draft (PM discussion → decisions Q1~Q5 반영) | AnYounggun |
