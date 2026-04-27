# iosession-lifetime-race Planning Document

> **Summary**: IOCP worker 가 freed `IOSession` 의 raw `this` 로 completion dispatch 하여 발생하는 UAF 크래시를, **Outstanding I/O Counter + Drain-before-Remove** 패턴으로 근본 해결한다. 세션의 단일 owner 는 container 이며, container 에서 `Remove` 되는 순간이 **유일한 결정적 소멸 시점** 이다. 모든 pending I/O 의 fail completion 이 돌아와 counter=0 이 된 마지막 completion 에서 `OnDisconnected()` 가 호출되고, 그 안에서 container.Remove → `~IOSession`. `iocp-game-server-engine` v1 의 **M1 선행 조건**.
>
> **Project**: FastPort
> **Version**: v1 (part of `iocp-game-server-engine`)
> **Author**: An Younggun
> **Date**: 2026-04-22 (v0.3 rewrite)
> **Status**: Draft (v0.3 — Outstanding Counter + Drain-before-Remove)
> **Parent Feature**: [`iocp-game-server-engine`](./iocp-game-server-engine.plan.md)

---

## Executive Summary

| Perspective | Content |
|-------------|---------|
| **Problem** | Stress 에서 IOCP worker 가 이미 freed 된 `IOSession` 의 raw `this` 로 `OnIOCompleted` dispatch → UAF. 관찰 증거(2026-04-22): `bResult=TRUE` 정상 completion 인데 `pConsumer=0xFFFFFFFFFFFFFFFF` — 세션 heap 이 해제되고 VM 페이지까지 반환된 깊이까지 race window 가 열려있음. `iocp-game-server-engine` v1 의 SC-3 (stress 1M×2 zero crash) 블로커이자 M2e (Recv 재설계) 의 선행 조건. |
| **Solution** | **단일 owner 원칙**: session 의 last owner = container. `IOSession::m_OutstandingIoCount` (atomic) 로 pending I/O 추적. posting 성공 시 +1, `OnIOCompleted` 진입 시 -1 (RAII scope-exit). `RequestDisconnect` 는 멱등 CAS + `shutdown(SD_BOTH)` + `closesocket()` 후 **비동기 return**. Windows 가 모든 pending I/O 에 fail completion 배송 → worker 스레드들이 순차 처리 → counter 가 1→0 전이하는 **마지막 completion** 에서만 `OnDisconnected()` 호출 → 내부에서 container.Remove → shared_ptr drop → `~IOSession`. late arrival 자체가 존재할 수 없다. SelfRetain 패턴(각 I/O 마다 shared_ptr 복제) 은 **명시적으로 기각** — 결정적 수명 관리가 목표. |
| **Function/UX Effect** | 외부 동작 무변화. 내부에서 세션 소멸이 **"모든 pending I/O drain 완료" 를 만족하는 유일한 시점** 에만 일어난다. Stress 3 시나리오 (A churn / B 1M×2 burst / C combined) 에서 UAF 0회. 상위 v1 의 M2e (Recv 재설계) 가 "OnIOCompleted 진입시 session 이 살아 있다" 는 불변식 위에 얹힐 수 있게 된다. |
| **Core Value** | v1 릴리즈 블로커 제거 + **결정적 세션 수명 모델** 확립. 이후 엔진 내 모든 비동기 경로(KeepAlive, ServerLifecycle, RecvBufferPool, PacketFramerCore) 가 "세션 = container-owned, Remove = 소멸" 단일 규칙 위에 설계된다. |

---

## Context Anchor

| Key | Value |
|-----|-------|
| **WHY** | Stress 에서 confirmed UAF (2026-04-22 증거: bResult=TRUE + pConsumer=0xFFFF…F). 상위 `iocp-game-server-engine` v1 의 SC-3 블로커이자 M2e 선행. |
| **WHO** | FastPort 엔진 유지보수자. 간접적으로 모든 FastPort 기반 게임/서비스 (한국 C++ 인디/미드코어 서버팀). |
| **RISK** | ① posting 실패 경로에서 counter 미복구 → counter 영구 drift ② RequestDisconnect 비동기로 late completion 쌓이면 worker 점유 ③ OnDisconnected 재진입 (다중 호출) ④ counter overhead 로 상위 v1 의 P99 80µs 영향 ⑤ Disconnect 대기 중 drain 이 무한 지연 |
| **SUCCESS** | Stress 3 시나리오 Debug+Release UAF 0회, 기존 `LibNetworksTests` 64 회귀 0, 세션 destruct 수 = accept 수 (leak 0), 3k conn idle throughput 감소 ≤3%, 64B P99 regression ≤5µs. |
| **SCOPE** | **IOCP `IOSession` 만**. RIO 쪽 동일 패턴은 v1.1 `riosession-lifetime-race` 로 분리. `IIOConsumer` 인터페이스 불변. Outstanding counter 는 IOSession 내부 field. OnDisconnected → container.Remove 는 기존 interface (injected weak_ref 또는 콜백) 로 연결. |

---

## 1. Overview

### 1.1 Purpose

`IOCPInboundSession` 세션이 disconnect 감지되어 `OnDisconnected` 가 container Remove 를 트리거하는 기존 흐름에서, **pending I/O 가 모두 drain 되기 전에 Remove 가 일어나면** IOCP worker 가 freed 메모리에 access 하여 UAF 가 발생한다. 본 feature 는 세션의 life-cycle 을 "container 가 유일한 owner" 원칙으로 단순화하고, Remove 시점을 "모든 pending I/O 의 completion 이 dequeue 된 마지막 순간" 으로 고정한다.

### 1.2 Background

- `IIOConsumer::GetCompletionId()` 는 `reinterpret_cast<ULONG_PTR>(this)` — raw pointer 를 completion key 로 사용.
- `IOService` worker 가 `GetQueuedCompletionStatus` 로 key 를 받아 raw pointer cast 후 `OnIOCompleted` 호출 → 이 시점에 세션이 alive 하다는 보장이 기존 코드에는 없다.
- 2026-04-22 Stress 재현 증거: `bResult=TRUE` 인 정상 completion 인데 `pConsumer=0xFFFFFFFFFFFFFFFF` — 세션 heap 해제는 물론 VM 페이지 반환까지 진행된 깊은 race.
- **표준 해법 2가지**:
  - (A) **SelfRetain**: 각 pending I/O 에 shared_ptr 복제를 묶어 자동 수명 연장 — `OverlappedEx` 에 shared_ptr 멤버
  - (B) **Outstanding Counter + Drain-before-Remove**: container 가 유일한 owner, pending I/O 갯수 atomic counter, counter==0 확인된 시점에만 container.Remove
- 본 프로젝트는 **(B) 를 선택**. (A) 는 "각 I/O 마다 owner 복제" 로 세션 수명이 비결정적(여러 shared_ptr 중 마지막 drop 시점에 소멸) 이 되어, 엔진 코드 리딩 시 "언제 destruct 되는가?" 에 대한 단일 답이 없다. (B) 는 "Remove 시점 = 유일한 destruct 시점" 으로 읽기 쉽고, game server 의 admin kick / timeout / error 경로 모두 동일한 흐름으로 수렴한다.

### 1.3 Related Documents

- **Parent Plan**: `docs/01-plan/features/iocp-game-server-engine.plan.md` — 본 feature 는 상위 v1 의 M1 선행 조건
- **Parent Design**: `docs/02-design/features/iocp-game-server-engine.design.md` — M2e `iosession integration` scope 가 본 feature 의 drain invariant 를 전제
- **Pre-patch Evidence**: `docs/evidence/iosession-lifetime-race-pre-patch-verify.md` (2026-04-22)
- **Prior Reports**:
  - `docs/04-report/server-status.report.md` §4.2 Future Work
  - `docs/04-report/session-idle-timeout.report.md`
  - `docs/04-report/global-timer-queue.report.md`
- **Touched source**:
  - `LibNetworks/IOSession.{ixx,cpp}` — **변경**
  - `LibNetworks/IOService.{ixx,cpp}` — **변경 없음** (IIOConsumer 불변)
  - `LibNetworks/RIOSession.*` — **본 feature 범위 밖** (v1.1 로 분리)
- **CLAUDE.md 규약**: Logger 표준, GMF `#include <spdlog/spdlog.h>`, Timer callback lifetime 규칙, 모듈 명명

---

## 2. Scope

### 2.1 In Scope (이번 PDCA)

- [ ] **S1 — Outstanding I/O Counter 도입 (IOSession)**
  - `std::atomic<int> m_OutstandingIoCount { 0 }` 멤버 추가
  - `std::atomic<bool> m_bDisconnecting { false }` 멤버 추가 (RequestDisconnect 멱등 CAS)
  - `std::atomic<bool> m_bOnDisconnectedFired { false }` 멤버 추가 (OnDisconnected 한 번만 호출 보장)
- [ ] **S2 — Posting 경로 counter 증가 + 실패 시 undo**
  - `RequestRecv` / `TryPostSendFromQueue`: WSARecv/WSASend 호출 직전 `fetch_add(1, acq_rel)`
  - 호출 결과가 `SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING` 이면 즉시 `fetch_sub(1, acq_rel)` → 만약 사전값 == 1 && `m_bDisconnecting` 이면 **Last-Completion 경로** 로 `TryFireOnDisconnected()` 호출
  - posting 실패 반환 후 caller 의 기존 실패 처리 (RequestDisconnect 등) 유지
- [ ] **S3 — Completion 경로 counter 감소 (RAII scope-exit)**
  - `OnIOCompleted` 진입 즉시 RAII `IoCompletionGuard` 생성 — destructor 가 `fetch_sub(1, acq_rel)` 후 사전값==1 && disconnecting 확인 시 `TryFireOnDisconnected()`
  - 모든 early return / exception 경로에서 자동 decrement 보장
- [ ] **S4 — RequestDisconnect 비동기 + 멱등**
  - `m_bDisconnecting.compare_exchange_strong(false, true)` CAS 로 **첫 호출만** 통과
  - 통과 시 `::shutdown(SD_BOTH)` + `::closesocket()` → Windows 가 모든 pending I/O 에 fail completion 배송 보장
  - **즉시 return** (완료 대기 안 함)
  - 두 번째 이후 호출은 무시 (이미 disconnecting)
- [ ] **S5 — Last-Completion 경로 (`TryFireOnDisconnected`)**
  - `m_OutstandingIoCount == 0 && m_bDisconnecting == true` 이며 `m_bOnDisconnectedFired.compare_exchange_strong(false, true)` 통과 시 `OnDisconnected()` 호출
  - `OnDisconnected()` 내부에서 container.Remove(sessionId) → container 의 shared_ptr drop → `~IOSession`
  - OnDisconnected 는 **정확히 한 번만** 호출됨 (CAS 보장)
- [ ] **S6 — Destructor paranoid check**
  - `~IOSession()`: `m_OutstandingIoCount == 0` assert (Debug) + LogError 로그 (Release)
  - Invariant 위반 시 즉시 발견 (논리상 절대 발생 안 해야 함)
- [ ] **S7 — Zero-byte Recv ↔ Real Recv 2단계 counter 독립 처리**
  - Zero-byte Recv posting: +1
  - Zero-byte completion: -1 (scope-exit)
  - Real Recv posting (zero-byte completion 분기 내 `RequestRecv(false)` 호출): 새 +1
  - Real Recv completion: -1 (scope-exit)
  - 각 I/O 독립적으로 counter 관리 (동일 Overlapped 재사용에도 논리적 독립)
- [ ] **S8 — L1 유닛 테스트** (`LibNetworksTests/IOSessionLifetimeTests.cpp`)
  - LT-01: posting 성공 시 counter == 1
  - LT-02: 시뮬 OnIOCompleted 후 counter == 0
  - LT-03: posting 실패 시 counter undo (0 복귀)
  - LT-04: RequestDisconnect 멱등 (10회 호출 → shutdown/closesocket 호출 수 1)
  - LT-05: Disconnect 후 pending completion drain → last completion 에서 OnDisconnected 1회만 호출
  - LT-06: 동시 Recv + Send 시뮬 → 두 completion 모두 처리된 이후에만 OnDisconnected 발사
  - LT-07: OnDisconnected 는 CAS 에 의해 정확히 1회만 (spurious double-fire 방지 테스트)
  - DT-01: destructor 시 counter==0 → ERROR 로그 없음
  - DT-02: (mock) destructor 시 counter!=0 → ERROR 로그 검출
- [ ] **S9 — Stress 재현 (`verify-after`)**
  - Scenario A / B / C 모두 Debug+Release 에서 UAF 0회 + leak 0
  - 세션 destruct count == accept count (로그 대조)
- [ ] **S10 — Perf baseline**
  - 3k conn idle throughput: 패치 전/후 측정. 감소 ≤ 3%
  - 64B echo P50/P99: 패치 전/후 측정. P99 regression ≤ 5µs
- [ ] **S11 — Logging 보강**
  - 세션 destruct 시 `m_OutstandingIoCount`, `m_bDisconnecting`, `m_bOnDisconnectedFired` 최종값 Debug 로그
  - RequestDisconnect 첫 통과 시 INFO 로그, 재호출 시 DEBUG 로그

### 2.2 Out of Scope

- ❌ **SelfRetain 패턴** (v0.2 의 shared_from_this 기반 OverlappedEx::SelfRetain). 본 v0.3 에서 **명시적 기각** — 결정적 수명 관리 원칙에 반함.
- ❌ RIO 쪽 동일 패턴 이식 (`RIOSession`). v1.1 `riosession-lifetime-race` 로 분리.
- ❌ `IIOConsumer` 인터페이스 변경
- ❌ `IOService` / `RIOService` 재작성
- ❌ Overlapped 를 per-I/O heap allocation 으로 전환
- ❌ `CancelIoEx` 명시 호출 (shutdown+close 전략 유지)
- ❌ Session leak 상시 모니터링 대시보드 (별 feature)
- ❌ 상위 v1 의 M2 (Zero-Copy Recv + RecvBufferPool) 구현 — 본 feature 는 Recv 경로 구조 변경 없음

---

## 3. Requirements

### 3.1 Functional Requirements

| ID | Requirement | Priority | Status |
|----|-------------|----------|--------|
| FR-01 | `IOSession` 에 `std::atomic<int> m_OutstandingIoCount`, `std::atomic<bool> m_bDisconnecting`, `std::atomic<bool> m_bOnDisconnectedFired` 멤버 추가 | High | Pending |
| FR-02 | `RequestRecv` posting 직전 counter +1, 실패(non-PENDING) 시 counter -1 복구 | High | Pending |
| FR-03 | `TryPostSendFromQueue` 동일 패턴 | High | Pending |
| FR-04 | `OnIOCompleted` 진입 시 RAII guard 생성 → exit 시 자동 counter -1 | High | Pending |
| FR-05 | Counter 가 1→0 전이 && disconnecting == true 시 `TryFireOnDisconnected()` 호출 | High | Pending |
| FR-06 | `RequestDisconnect` 멱등: `m_bDisconnecting` CAS → shutdown + closesocket 후 비동기 return | High | Pending |
| FR-07 | `TryFireOnDisconnected()` 는 `m_bOnDisconnectedFired` CAS 로 1회만 `OnDisconnected()` 호출 | High | Pending |
| FR-08 | `OnDisconnected()` 내부에서 container.Remove(sessionId) 실행 → shared_ptr drop → `~IOSession` | High | Pending |
| FR-09 | `~IOSession()` 에서 counter==0 assert + ERROR 로그 | High | Pending |
| FR-10 | `LibNetworksTests/IOSessionLifetimeTests.cpp` LT-01~07 + DT-01~02 (총 9 tests) | High | Pending |
| FR-11 | Stress reproducer 3 시나리오 (이전 `--scope reproducer` 에서 완료) + `verify-after` 에서 Debug+Release UAF 0회 | High | Pending |
| FR-12 | 3k conn idle throughput + 64B P99 패치 전/후 비교 리포트 | Medium | Pending |
| FR-13 | RIO 변경 파일 0 건 (grep 검증) | Medium | Pending |
| FR-14 | Session destruct 로그에 counter/disconnecting/fired 최종값 포함 (Debug) | Medium | Pending |

### 3.2 Non-Functional Requirements

| Category | Criteria | Measurement Method |
|----------|----------|-------------------|
| **Safety** | Stress 3 시나리오 Debug+Release UAF 크래시 0회 | Stress reproducer + CRT heap check |
| **Leak** | Stress 종료 후 세션 leak 0 (destruct 로그 = accept 로그) | Logger grep + `_CRTDBG_LEAK_CHECK_DF` |
| **Regression** | `LibNetworksTests` 기존 64 회귀 0 | `vstest.console.exe` |
| **Throughput** | 3k conn idle 감소 ≤ 3% | 기존 benchmark 비교 |
| **Latency** | 64B echo P99 regression ≤ 5µs | `FastPortBenchmark` or 상위 v1 `bench_echo.ps1` |
| **Memory** | 세션당 atomic<int> + atomic<bool>×2 = +8 bytes (x64) | 코드 리뷰 |
| **Build** | MSVC x64 Debug/Release warning-as-error green | `FastPort.slnx` |
| **Compat** | `IIOConsumer` / `INetworkSession` / `OutboundSession` / `InboundSession` ABI 불변 | 헤더 diff |
| **Determinism** | 모든 세션의 `~IOSession` 가 container.Remove 호출 **이후** 에만 발생하는지 로그로 검증 | 2개 이상 세션에 대해 destruct 전후 로그 순서 확인 |

---

## 4. Success Criteria

### 4.1 Definition of Done

- [ ] FR-01 ~ FR-14 구현/검증 완료
- [ ] L1 신규 테스트 LT-01~07 + DT-01~02 전부 green
- [ ] `LibNetworksTests` 전체 회귀 0
- [ ] Stress Scenario A (10k × 5분 churn) Debug+Release UAF 0회
- [ ] Stress Scenario B (1M × 2 round burst) Debug+Release UAF 0회
- [ ] Stress Scenario C (1k conn × 100 pps × 5min) Debug+Release UAF 0회
- [ ] 세션 leak 0 (`_CRTDBG_LEAK_CHECK_DF` + 로그 대조)
- [ ] 3k conn idle throughput 감소 ≤ 3%
- [ ] 64B P99 regression ≤ 5µs
- [ ] Design 문서 §6 Lifetime Invariants + counter state diagram 존재
- [ ] RIO 변경 파일 grep 0 건
- [ ] 상위 `iocp-game-server-engine.plan.md` §2.1 M1 unblocked 업데이트

### 4.2 Quality Criteria

- [ ] `warning-as-error` 유지 (경고 0)
- [ ] Logger 표준 준수 (spdlog 직접 호출 0)
- [ ] GMF `#include <spdlog/spdlog.h>` 유지
- [ ] Timer callback lifetime 규칙 유지 (본 feature 에서 TimerQueue 경로 변경 없음)
- [ ] **결정적 수명 모델 준수** — session destruct 시점이 로그 상 항상 container.Remove 호출 이후임
- [ ] `m_bOnDisconnectedFired` CAS 로 OnDisconnected 가 정확히 1회만 호출됨 (유닛 테스트로 검증)

---

## 5. Risks and Mitigation

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| **R1. Posting 실패 경로에서 counter 미복구 → 영구 drift → session 영영 소멸 안 됨** | High | Medium | posting 코드 경로마다 fetch_add 와 fetch_sub 쌍을 표로 매핑 후 코드 리뷰. 유닛 테스트 LT-03 로 잠금. |
| **R2. OnIOCompleted 내부 early return 에서 counter decrement 누락** | High | Medium | **RAII `IoCompletionGuard` 의무화** — destructor 가 무조건 fetch_sub. 추가 수동 decrement 금지 규약. |
| **R3. OnDisconnected 중복 호출** | High | Low | `m_bOnDisconnectedFired` CAS 로 정확히 1회 보장. 유닛 테스트 LT-07. |
| **R4. Counter fetch_add/sub atomic overhead 로 P99 80µs 영향** | Medium | Low | 5~10ns × 2 (add+sub) 수준. 3k conn × 1 Hz = 6k ops/s → 무시 가능. 측정 후 문제 시 `memory_order_relaxed` 로 완화 검토. |
| **R5. Disconnect 후 pending I/O drain 이 무한 지연** | Medium | Low | Windows 가 closesocket 후 반드시 fail completion 배송 보장. TCP RST 미도착 시에도 local close 는 완료. 극단적 경우 상위 ServerLifecycle drain timeout (v1 M4) 에서 force join. |
| **R6. posting 성공/실패 경계 시 동시성 race** (fetch_add 했는데 WSARecv 가 동기 완료되어 OnIOCompleted 가 먼저 실행) | Medium | Low | IOCP semantics: WSARecv 가 `WSA_IO_PENDING` 이 아니면 completion 은 IOCP 에 queue 됨 (FILE_SKIP_COMPLETION_PORT_ON_SUCCESS 미설정 상태). fetch_add 가 queue 이전이므로 순서 역전 없음. `memory_order_acq_rel` 로 강화. |
| **R7. shutdown(SD_BOTH) 가 일부 OS 설정에서 completion 미배송** | High | Low | 테스트 환경에서 확인. 미배송 시 CancelIoEx 보조 호출로 전환. |
| **R8. Counter 가 Send/Recv 구분 없이 합쳐져 있어 디버깅 어려움** | Low | Medium | Debug 로그에서 RequestRecv/TryPostSendFromQueue posting 시 +1 with source, OnIOCompleted 시 -1 with source 출력 (Debug 빌드 only). |

---

## 6. Impact Analysis

### 6.1 Changed Resources

| Resource | Type | Change |
|----------|------|--------|
| `LibNetworks/IOSession.ixx` | Class def | `m_OutstandingIoCount` (atomic<int>), `m_bDisconnecting` (atomic<bool>), `m_bOnDisconnectedFired` (atomic<bool>), `TryFireOnDisconnected()` helper 선언 |
| `LibNetworks/IOSession.cpp` | Impl | RequestRecv / TryPostSendFromQueue posting counter, OnIOCompleted RAII guard, RequestDisconnect CAS + shutdown/closesocket, TryFireOnDisconnected, ~IOSession paranoid check |
| `LibNetworksTests/IOSessionLifetimeTests.cpp` | New | LT-01~07 + DT-01~02 |
| `FastPortTestClient` Stress mode | 변경 없음 | 이미 완료 (`--scope reproducer`) |
| `docs/01-plan/features/iocp-game-server-engine.plan.md` | Doc | 본 feature 완료 시 M1 unblocked 업데이트 |
| `LibNetworks/RIOSession.*` | **변경 없음** | v1.1 로 분리 |
| `LibNetworks/IOService.*` / `RIOService.*` | **변경 없음** | IIOConsumer 불변 |

### 6.2 Current Consumers

| Resource | Caller | Impact |
|----------|--------|--------|
| `IOSession::OverlappedEx` | `IOSession` 내부 only | None — 멤버 추가 없음 (v0.3 에서 SelfRetain 기각) |
| `IOSession::OnIOCompleted` | `IOService` worker | None — 시그니처 불변, 내부에 RAII guard 추가 |
| `IOSession::RequestDisconnect` | `IOCPInboundSession::OnDisconnected` 호출 경로, Admin Kick, IdleTimeout | **Semantics 강화**: 멱등 + 비동기. 기존 호출부는 수정 불필요 (이미 idempotent 가정) |
| `IOSession::OnDisconnected` (virtual) | `IOCPInboundSession::OnDisconnected` override | **호출 시점 변경**: 기존엔 RequestDisconnect 경로에서 즉시 호출 가능. v0.3 에서는 **마지막 completion 에서만** 호출. `IOCPInboundSession::OnDisconnected` 내부의 container.Remove 로직은 그대로 유지 |
| `IOCPInboundSession::OnDisconnected` | `FastPortServer` / container | None — 내부 로직 불변 (여전히 container.Remove 실행) |
| `IIOConsumer` | `IOService` | None — 시그니처 불변 |
| `IdleTracker` / `IIdleAware` | `IOSession` idle timeout 경로 | None — disconnect 경로는 `RequestDisconnect` 를 사용 (멱등 보장됨) |

### 6.3 Verification

- [ ] `IIOConsumer` 헤더 diff 0
- [ ] `INetworkSession` / `OutboundSession` / `InboundSession` 시그니처 불변
- [ ] `LibCommons::TimerQueue` / `Logger` 영향 없음
- [ ] `LibNetworksRIO` / `FastPortServerRIO` 변경 0 파일
- [ ] FastPortTestClient 기존 기능 회귀 없음
- [ ] IdleTracker 기존 동작 유지 (timeout → RequestDisconnect 경로)
- [ ] Admin Kick 경로 회귀 없음

---

## 7. Architecture Considerations

### 7.1 Project Context

FastPort 는 Windows C++20 모듈 기반 IOCP/RIO 게임 서버 엔진. 프로젝트 레벨: **Enterprise (Native C++)**.

### 7.2 Key Architectural Decisions

| Decision | Options | Selected | Rationale |
|----------|---------|----------|-----------|
| 수명 관리 방식 | (A) SelfRetain (shared_ptr per I/O) / (B) Outstanding Counter + Drain | **(B) Outstanding Counter + Drain-before-Remove** | 단일 owner 원칙 준수. session destruct 시점이 "container.Remove" 한 곳으로 결정. 엔진 리딩/디버깅 단순화. v0.3 사용자 결정 |
| RequestDisconnect 동기성 | 동기 / **비동기** | **비동기** | 호출자 non-blocking. admin kick / idle timeout 경로가 여러 세션 동시 종료 시에도 직렬화 위험 없음 |
| Container Remove 호출자 | **IOSession 내부 (OnDisconnected)** / 외부 reaper | **IOSession 내부** | 캡슐화. session 이 자기 여정을 스스로 마침. container weak_ptr 또는 callback 으로 연결 |
| Counter 단위 | Send/Recv 통합 vs 분리 | **통합 (하나)** | drain 판정은 "모든 I/O 끝났는가" 하나. Debug 로그에서 source(R/S) 만 구분 |
| Zero-byte ↔ Real Recv 경로 | 단일 counter / 각각 별도 fetch | **각 I/O 별 fetch** | 명확. Zero-byte completion → Real Recv posting 시 자연스럽게 -1 → +1 |
| RAII guard | 수동 / **RAII scope-exit** | **RAII `IoCompletionGuard`** | CLAUDE.md 의 `lock_guard` 선호 정책 연장. exit path 누락 불가능 |
| OnDisconnected 중복 방지 | flag / mutex / CAS | **`atomic<bool>` CAS** | lock-free, 1회 fire 확정 |
| RIO 포함 | 포함 / 분리 | **분리 (v1.1)** | 상위 v1 의 RIO freeze 결정 준수 |

### 7.3 Counter Life-cycle (설계 요약)

```
상태 전이:
  [Alive, count=0, !disc]
      ↓ posting (RequestRecv / TryPostSendFromQueue)
  [Alive, count>=1, !disc]
      ↓ OnIOCompleted ×N (자연 반복, count 오르내림)
  [Alive, count>=1, !disc]
      ↓ RequestDisconnect (CAS first pass)
  [Disconnecting, count>=1, disc]     ← shutdown+close 완료
      ↓ 각 pending I/O 가 fail completion 으로 돌아와 fetch_sub
  [Disconnecting, count>0, disc]
      ↓ 마지막 completion fetch_sub(1) → prev==1 감지
  [Draining-last, count=0, disc]
      ↓ TryFireOnDisconnected() CAS (first pass)
  [Fired, count=0, disc, fired]
      ↓ OnDisconnected() → container.Remove(id)
  [Removed] ← container shared_ptr drop → ~IOSession()
```

---

## 8. Stress Reproducer Spec (재사용)

`--scope reproducer` 에서 이미 3 시나리오 구현 완료. 본 feature 는 이를 **검증 도구** 로 재활용.

| Scenario | 목적 | 동시 연결 | 패턴 | 실행 시간 |
|---|---|---|---|---|
| A. Churn | Accept/Close race | 10,000 | 초당 100 DC+RC | 5 분 |
| B. Burst | 단일 세션 대량 I/O + SC-3 | 1~4 | 1M × 2 round | 라운드 별 자동 종료 |
| C. Combined | 복합 부하 | 1,000 | 100 pps/세션 | 5 분 |

### 공통 검증

- UAF 크래시 0 (`_CrtSetDbgFlag(_CRTDBG_CHECK_ALWAYS_DF | _CRTDBG_LEAK_CHECK_DF)`)
- 세션 destruct 로그 수 = accept 로그 수
- x64 Debug + Release 양쪽
- destruct 로그가 항상 "container.Remove(id)" 로그 이후에 출력됨 (결정적 수명 증명)

---

## 9. Parent Feature Integration

본 feature 는 `iocp-game-server-engine` v1 Plan 의 **M1** 과 1:1 매핑.

| Parent Module | Parent Scope Key | 본 feature 와의 관계 |
|---|---|---|
| M1 | `m1-lifetime-race` | **= 본 feature** |
| M2e | `m2e-iosession` | Recv 경로 RecvBufferPool + PacketFramerCore 재설계. **Drain invariant 가 있어야** "OnIOCompleted 진입시 session alive" 라는 전제가 성립. 본 feature 가 main 에 먼저 병합 필수 |
| M4 | `m4-lifecycle` | `ServerLifecycle::Shutdown()` drain 단계가 session 의 counter/disconnecting 불변식을 그대로 활용 |
| M5 | `m5c-bench-local` | 본 feature 의 3 시나리오 reproducer 는 상위 벤치 인프라와 공유 |

---

## 10. Convention Prerequisites

| Rule | 적용 |
|---|---|
| CLAUDE.md — Logger 표준 | ✅ 준수. Debug 빌드만 추가 로그 |
| CLAUDE.md — GMF `#include <spdlog/spdlog.h>` | ✅ `IOSession.cpp` 에 유지/추가 |
| CLAUDE.md — Timer callback lifetime | ✅ 본 feature TimerQueue 경로 불변 |
| CLAUDE.md — Shutdown 순서 | ✅ 본 feature 변경 없음 |
| CLAUDE.md — 네이밍 (`m_`, `k`, `::` Win32) | ✅ 준수 (`m_OutstandingIoCount`, `m_bDisconnecting`, `kLogCategoryIOSession`) |
| CLAUDE.md — `std::lock_guard` / RAII 선호 | ✅ `IoCompletionGuard` 신설 |

---

## 11. Next Steps

1. [ ] `/pdca design iosession-lifetime-race` — Design v0.3 재작성 (Outstanding Counter + Drain 기반)
2. [x] `/pdca do iosession-lifetime-race --scope reproducer` — 완료 (3 시나리오)
3. [x] `/pdca do iosession-lifetime-race --scope verify-before` — 완료 (증거 확보)
4. [ ] `/pdca do iosession-lifetime-race --scope iocp-patch` — IOSession 에 counter/disc/fired 도입 + RAII guard + CAS 멱등
5. [ ] `/pdca do iosession-lifetime-race --scope tests` — LT-01~07 + DT-01~02
6. [ ] `/pdca do iosession-lifetime-race --scope verify-after` — A/B/C × Debug/Release 재실행 + perf 측정
7. [ ] `/pdca analyze iosession-lifetime-race`
8. [ ] `/pdca report iosession-lifetime-race`
9. [ ] 완료 후 상위 `iocp-game-server-engine.plan.md` 의 M1 unblocked 업데이트 → `--scope m2e-iosession` 진입

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-04-21 | Initial draft (Q1~Q10, IOCP+RIO 동시 수정, 10k×5min stress) | AnYounggun |
| 0.2 | 2026-04-22 | v1 통합 재작성 — `iocp-game-server-engine` M1 편입, RIO 분리, Stress 3 시나리오 병합, **SelfRetain 패턴 채택** | An Younggun |
| **0.3** | **2026-04-22** | **방향 전환 재작성 — SelfRetain 명시 기각. Outstanding I/O Counter + Drain-before-Remove 패턴 채택. 단일 owner(container) 원칙. RequestDisconnect 비동기 멱등 CAS. Last-completion 에서 OnDisconnected CAS 1회 fire. RAII `IoCompletionGuard`. Zero-byte / Real Recv counter 독립. 검증 스코프 reproducer/verify-before 완료 표시.** | **An Younggun** |
