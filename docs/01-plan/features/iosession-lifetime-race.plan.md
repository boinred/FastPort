# iosession-lifetime-race Planning Document

> **Summary**: IOCP/RIO worker 가 raw `this` pointer 로 `OnIOCompleted` dispatch 중 세션이 이미 freed 되어 발생하는 use-after-free 를 Outstanding-I/O self-retain 패턴으로 근본 해결.
>
> **Project**: FastPort
> **Author**: AnYounggun
> **Date**: 2026-04-21
> **Status**: Draft

---

## Executive Summary

| Perspective | Content |
|-------------|---------|
| **Problem** | 3000+ 동시 연결 stress 에서 `IOSession::m_RecvOverlapped.WSABufs.push_back` 크래시. 디버거 상 `this` 가 `0xdddddddddddddddd` (MSVC CRT freed-heap fill) — IOCP worker 가 이미 소멸된 세션 객체의 freed memory 를 건드림. RIO 도 동일 구조로 잠재 취약. |
| **Solution** | **Outstanding-I/O self-retain**: `OverlappedEx` (IOCP) / Send/Receive context (RIO) 에 `std::shared_ptr<Session>` 멤버 추가. I/O posting 직전에 `shared_from_this()` 로 retain, `OnIOCompleted` (또는 RIO completion 처리) 에서 처리가 끝난 후 reset. 이로써 container 에서 제거되어도 pending I/O 완료 시점까지 세션이 alive 보장. |
| **Function/UX Effect** | 외부 기능/UX 무변화. 내부 안정성만 향상. 10k 동시 연결 stress 에서 5분간 재연결 반복해도 UAF 크래시 0회. 기능 테스트 64/64 회귀 0. |
| **Core Value** | FastPort 가 장기 실행 Windows Service 로서 요구되는 **런타임 안정성** 확보. 부하 테스트/프로덕션 배포 시 디버깅 비용 감소. IOCP 표준 retain 패턴을 도입하여 향후 세션 구조 확장 시에도 재사용 가능. |

---

## Context Anchor

| Key | Value |
|-----|-------|
| **WHY** | 3000 conn stress 에서 confirmed use-after-free 크래시 (freed heap fill 증거). 런타임 안정성 직결 — 프로덕션/장기 배포 블로커. |
| **WHO** | FastPort 엔진 유지보수자 (직접 영향). 간접적으로 모든 FastPort 기반 게임/서비스. |
| **RISK** | 세션 lifetime 구조 변경이 기존 `RequestDisconnect` / `OnDisconnected` 흐름과 충돌할 위험, 순환 참조 (self ← self-retain) 로 영원히 소멸 안 되는 세션, shared_ptr ref-count 경합으로 인한 성능 저하. |
| **SUCCESS** | 10k conn × 5분 stress 중 UAF 크래시 0회 (Debug·Release 양쪽), 64개 기존 L1 회귀 0, 기능 외부 동작 무변화, 세션 destruct 정상 발생 (leak 0). |
| **SCOPE** | IOCP `IOSession` + RIO `RIOSession` 양쪽 동시 수정. `IIOConsumer` 인터페이스 불변. 10k conn stress reproducer 도구 포함. |

---

## 1. Overview

### 1.1 Purpose

`IOCPInboundSession` 세션이 disconnect 되어 container 에서 shared_ptr ref 가 0으로 떨어지는 순간, IOCP worker 가 이미 dequeue 한 completion 으로 **freed 메모리의 `this`** 에 접근하여 발생하는 UAF 크래시를 근본적으로 제거한다. RIO 도 동일 패턴이므로 같이 수정한다.

### 1.2 Background

- `IIOConsumer::GetCompletionId()` 는 `reinterpret_cast<ULONG_PTR>(this)` — **raw pointer** 를 completion key 로 씀.
- `IOService` 의 worker 가 `GetQueuedCompletionStatus` 로 key 를 받아 raw pointer cast 후 `OnIOCompleted` 호출 → 이 시점에 세션이 alive 하다는 보장 없음.
- RIO 도 `RIORequestContext` 에 raw pointer 저장하여 동일 구조.
- server-status 의 3000 conn 부하 테스트 중 디버거 스크린샷: `_Wptr._Ptr == 0xdddddddddddddddd` (CRT `_bDeadLandFill`), `m_RecvOverlapped.WSABufs.push_back` 에서 AV.
- **표준 IOCP 해법**: I/O 가 kernel 에 posting 되는 순간부터 completion 이 돌아올 때까지 **세션 자체 수명을 연장**. 가장 가벼운 방법은 Overlapped context 에 `shared_ptr` 를 묶어 두는 것.

### 1.3 Related Documents

- 선행 증거: `docs/04-report/server-status.report.md` §4.2 Future Work
- 영향 파일 선행 리포트:
  - `docs/04-report/session-idle-timeout.report.md` (IIdleAware 추가)
  - `docs/04-report/global-timer-queue.report.md` (TimerQueue 라이프사이클 교훈)
- 관련 소스: `LibNetworks/IOSession.{ixx,cpp}`, `LibNetworks/RIOSession.{ixx,cpp}`, `LibNetworks/IOService.{ixx,cpp}`, `LibNetworks/RIOService.{ixx,cpp}`

---

## 2. Scope

### 2.1 In Scope (이번 PDCA)

- [ ] `IOSession::OverlappedEx` 에 `std::shared_ptr<IOSession> SelfRetain` 멤버 추가
- [ ] `IOSession::RequestRecv` / `TryPostSendFromQueue` — WSARecv/WSASend 호출 **직전** `SelfRetain = shared_from_this()`, posting 실패 시 reset
- [ ] `IOSession::OnIOCompleted` — Recv / Send 각 분기 return 직전 `SelfRetain` reset (모든 exit path 커버)
- [ ] `RIOSession` 동일 패턴 — RIO Send/Receive context 에 `std::shared_ptr<RIOSession> SelfRetain` (Q8=a 결정, raw this 는 유지)
- [ ] `RIOSession` 의 `RequestRecv` / Send posting 경로 및 completion 처리 경로에서 retain/reset
- [ ] 순환 참조 방지 검증: disconnect 경로에서 반드시 reset 되어 self-ref 가 끝날 수 있는 구조
- [ ] 기존 `IIOConsumer` 인터페이스 및 `IOService` / `RIOService` 는 **변경 없음** (Q7=a)
- [ ] 기존 `IOCPInboundSession::OnDisconnected` / `RIOInboundSession::OnDisconnected` 의 `sessions.Remove()` 타이밍 **유지** (Q9=a — retain 이 수명 보장)
- [ ] 단위 테스트:
  - `LibNetworksTests/IOSessionLifetimeTests.cpp` — 4~6 테스트
    - post-and-free race 시뮬: I/O 에뮬레이트 중 container 제거 → retain 덕에 완료 후 소멸
    - retain 이 완료 시 반드시 reset 되어 leak 없음
    - posting 실패 시 retain 이 동기로 해제
  - `LibNetworksTests/RIOSessionLifetimeTests.cpp` — 유사 구조
- [ ] **Stress reproducer**: `FastPortTestClient` Scale 탭 또는 전용 모드 확장
  - 10k 동시 연결 수립
  - 5분 동안 반복 disconnect + reconnect (초당 일정 수)
  - UAF 크래시 0회 / 세션 leak 0 확인
- [ ] **Release 재현 선행**: 패치 전에 Release|x64 로도 UAF 크래시 재현 여부 확인 (Q4) — 증상 기록
- [ ] 로깅: session destruct 시 outstanding I/O count 로그 (leak 조기 발견)

### 2.2 Out of Scope

- `IIOConsumer` 인터페이스 변경 / completion key 를 `weak_ptr` heap holder 로 변경 (Q7=b 안은 보류)
- IOService 전면 재작성
- Overlapped 를 per-I/O heap allocation 으로 전환 (현재 멤버 재사용 구조 유지)
- `CancelIoEx` 명시 호출 (Q6=a — Shutdown+Close 로 자연 cleanup)
- Container 제거 시점 지연 / outstanding counter (Q9=b 안은 보류)
- Session leak 상시 모니터링 대시보드 (후속 피처)
- Auth/admin 등 server-status Phase 2 항목

---

## 3. Requirements

### 3.1 Functional Requirements

| ID | Requirement | Priority | Status |
|----|-------------|----------|--------|
| FR-01 | `IOSession::OverlappedEx::SelfRetain` 멤버 추가 (Recv/Send 각각) | High | Pending |
| FR-02 | IOCP `RequestRecv` posting 직전 `SelfRetain = shared_from_this()`, 실패 시 reset | High | Pending |
| FR-03 | IOCP `TryPostSendFromQueue` 동일 패턴 | High | Pending |
| FR-04 | IOCP `OnIOCompleted` Recv 분기 (성공/실패/zero-byte/real/commit-fail) 모든 exit 에서 `m_RecvOverlapped.SelfRetain` reset | High | Pending |
| FR-05 | IOCP `OnIOCompleted` Send 분기 모든 exit 에서 `m_SendOverlapped.SelfRetain` reset | High | Pending |
| FR-06 | `RIOSession` Send/Receive context 에 `SelfRetain` 멤버 추가 + posting/completion 대응 | High | Pending |
| FR-07 | `RequestDisconnect` 경로에서 retain 관련 추가 reset 불필요 검증 (completion 이 자연 도착하여 처리) | High | Pending |
| FR-08 | 순환 참조 검증: disconnect 후 모든 completion 도착 시 세션 destruct 실제 발생하는지 테스트 | High | Pending |
| FR-09 | 단위 테스트: IOSession/RIOSession lifetime 시뮬 (TestableIOSession 패턴 재사용) | High | Pending |
| FR-10 | Stress reproducer 도구: 10k conn × 5분 reconnect 반복 (FastPortTestClient 확장) | High | Pending |
| FR-11 | 세션 destruct 시 outstanding I/O count 로그 (`"IOSession"`/`"RIOSession"` 카테고리) | Medium | Pending |
| FR-12 | Release|x64 재현 확인 (패치 전) 및 결과 기록 | Medium | Pending |

### 3.2 Non-Functional Requirements

| Category | Criteria | Measurement Method |
|----------|----------|-------------------|
| **Safety** | 10k conn × 5분 stress 중 UAF 크래시 0회 (Debug + Release) | Stress reproducer, CRT heap check |
| **Leak** | Stress 종료 후 세션 leak 0 (destruct 로그 수 = accept 로그 수) | Logger grep |
| **Regression** | LibNetworksTests 64 개 기존 테스트 회귀 0 | `vstest.console.exe _Builds/x64/Debug/LibNetworksTests.dll` |
| **Performance** | 3000 conn idle 상태에서 throughput 감소 ≤ 3% (atomic ref-count 오버헤드) | 기존 benchmark 비교 |
| **Memory overhead** | 세션당 `shared_ptr` × 2 = 16 bytes (x64) 추가 | 코드 리뷰 |
| **Build** | MSVC x64 Debug/Release 모두 빌드 | `FastPort.slnx` |

---

## 4. Success Criteria

### 4.1 Definition of Done

- [ ] FR-01 ~ FR-12 모두 구현 완료
- [ ] L1 신규 테스트 (IOSessionLifetimeTests + RIOSessionLifetimeTests) 전부 green
- [ ] LibNetworksTests 전체 회귀 0
- [ ] Stress reproducer (10k conn × 5분 reconnect 반복) Debug + Release 양쪽에서 UAF 크래시 0회
- [ ] Stress 종료 후 세션 leak 0 확인 (로그 대조)
- [ ] `_CrtSetDbgFlag(_CRTDBG_LEAK_CHECK_DF)` 하 Debug 실행 leak 보고 0
- [ ] Design document 의 §6 Lifetime Invariants 섹션에 retain/reset 규약 명문화
- [ ] Code review (self-review OK)

### 4.2 Quality Criteria

- [ ] 신규 shared_ptr retain 관련 assert/로그가 reset 누락을 조기 발견
- [ ] 순환 참조 없음: `ssion` 의 self-retain 이 I/O 완료 시 반드시 해제되는 invariant
- [ ] 모든 exit path (early return / catch / commit fail) 에서 retain 관리 누락 없음
- [ ] 빌드 경고 0 (warning-as-error 유지)

---

## 5. Risks and Mitigation

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| **순환 참조로 세션 영원히 소멸 안 됨** | High | Medium | Disconnect 경로에서 `Shutdown+Close` 하면 pending I/O 가 반드시 실패 completion 으로 돌아옴 → `OnIOCompleted` 에서 retain reset 보장. 테스트로 검증 (FR-08). |
| **Reset 누락된 exit path 존재** | High | Medium | `OnIOCompleted` 의 Recv/Send 분기 모든 return 경로를 디자인 단계에서 표 형태로 매핑. RAII helper (scope-exit) 사용 고려. |
| **shared_ptr atomic ref-count 성능 저하** | Medium | Low | 세션 수 × 패킷률 × 2 (retain + reset) 수준. 3k conn × 1Hz = 6k ops/s → 무시 가능. 이상 시 `intrusive_ptr` 로 전환 검토. |
| **Stress 가 UAF 재현 실패 (patch 검증 불가)** | High | Low | 패치 **전** reproducer 로 UAF 재현 확인을 DoD 필수 항목화 (FR-12). Debug CRT heap fill + breakpoint on UAF. |
| **RIO 쪽 retain 패턴이 RIO request context 규약과 충돌** | Medium | Low | Q8=a 결정: RIO context 는 기존 raw this 유지, retain 은 session 멤버로. RIO BufferId/RIO_BUF 제약 영향 없음. |
| **IOCPInboundSession 의 container Remove 타이밍 기존 코드 변경** | Low | Low | Q9=a: 변경 없음. retain 이 수명 연장하므로 Remove 즉시도 안전. |

---

## 6. Impact Analysis

### 6.1 Changed Resources

| Resource | Type | Change |
|----------|------|--------|
| `LibNetworks/IOSession.ixx` | Class definition | `OverlappedEx::SelfRetain` 멤버 추가 |
| `LibNetworks/IOSession.cpp` | Impl | RequestRecv / TryPostSendFromQueue / OnIOCompleted 에 retain/reset |
| `LibNetworks/RIOSession.ixx` | Class definition | Send/Receive request context 에 SelfRetain |
| `LibNetworks/RIOSession.cpp` | Impl | posting / completion 경로에 retain/reset |
| `LibNetworksTests/IOSessionLifetimeTests.cpp` | New file | 신규 단위 테스트 |
| `LibNetworksTests/RIOSessionLifetimeTests.cpp` | New file | 신규 단위 테스트 |
| `FastPortTestClient/TestClientApp.ixx` + `TestRunner.ixx` | Stress mode | 10k × 5분 reconnect 시나리오 |

### 6.2 Current Consumers (영향 확인)

| Resource | Caller | Impact |
|----------|--------|--------|
| `IOSession::OverlappedEx` | `IOSession` 내부만 — protected 이며 현재 서브클래스 없음 | None — 멤버 추가만, 기존 접근 경로 불변 |
| `IOSession::OnIOCompleted` | `IOService` worker 의 `GetQueuedCompletionStatus` 경로 | None — 시그니처 불변, 내부 추가 동작만 |
| `RIOSession::Send/Receive` | `RIOService` completion loop, `RIOInboundSession` | None — 시그니처 불변 |
| `IOCPInboundSession` / `RIOInboundSession` | `FastPortServer` / `FastPortServerRIO` ServiceMode | None — 세션 구현 디테일 변경, 인터페이스 불변 |
| `FastPortTestClient` Scale 테스트 | UI 버튼 한 개 | 옵션 추가 (10k × 5min reconnect), 기존 모드 유지 |

### 6.3 Verification

- [ ] `IIOConsumer` 시그니처 불변 — IOService 와의 계약 무영향
- [ ] `INetworkSession` / `OutboundSession` / `InboundSession` 시그니처 불변
- [ ] 기존 FastPortTestClient 일반 기능 (Echo/Benchmark/Metrics/Admin) 영향 없음
- [ ] LibCommons `TimerQueue` / `Logger` 무영향

---

## 7. Architecture Considerations

### 7.1 Project Context

FastPort 는 Windows C++20 모듈 기반 IOCP/RIO 게임 서버 엔진. 프로젝트 Level: **Custom Native (Enterprise 유사)**. Next.js/React 등 표준 Dynamic 템플릿 적용 대상 아님.

### 7.2 Key Architectural Decisions

| Decision | Options | Selected | Rationale |
|----------|---------|----------|-----------|
| Retain 단위 | (a) Recv/Send 독립 shared_ptr / (b) 단일 outstanding counter | **(a)** | IOCP 가 Recv/Send 별도 Overlapped 로 처리하므로 자연스러움 (Q5=a). 단순, RAII 친화. |
| Disconnect 시 pending I/O 처리 | (a) Shutdown+Close 로 자연 fail completion / (b) CancelIoEx | **(a)** | 기존 흐름 유지 (Q6=a). Windows 가 보장하는 failure completion 으로 retain 자연 해제. |
| 인터페이스 변경 | (a) IOSession 내부만 / (b) IIOConsumer 를 weak_ptr 기반으로 | **(a)** | IOService 무영향, 리스크 최소 (Q7=a). 더 근본적 수정은 후속 과제로 유보. |
| RIO context 표현 | (a) raw this + session 멤버 retain / (b) heap struct { shared_ptr, kind } | **(a)** | IOCP 대칭 유지 (Q8=a). RIO_BUFFERID/제약 영향 없음. |
| Container Remove 타이밍 | (a) OnDisconnected 즉시 / (b) outstanding counter 0 대기 | **(a)** | retain 이 수명 보장하므로 Remove 즉시도 안전 (Q9=a). 단순성 유지. |
| Stress reproducer | 10k conn / 5min / reconnect 반복 | Q10 결정 | 실제 프로덕션과 유사한 부하로 재현 신뢰도 확보 |

### 7.3 Implementation Notes

```
┌────────────────────────────────────────────────────────┐
│  Posting phase (RequestRecv / SendMessage path)         │
│  ──────────────────────────────────────────────────────│
│    m_RecvOverlapped.SelfRetain = shared_from_this();   │
│    int r = ::WSARecv(...);                              │
│    if (r == SOCKET_ERROR && err != WSA_IO_PENDING) {   │
│       m_RecvOverlapped.SelfRetain.reset();              │
│       return false;                                     │
│    }                                                    │
│                                                         │
│  Completion phase (OnIOCompleted, each exit path)       │
│  ──────────────────────────────────────────────────────│
│    auto self = std::move(m_RecvOverlapped.SelfRetain); │
│    // ... existing recv/send handling ...              │
│    return;  // self local drops here                   │
└────────────────────────────────────────────────────────┘
```

**Invariant**: 한 시점에 `m_RecvOverlapped.SelfRetain` 은 단 하나의 pending WSARecv 에만 대응. `m_RecvInProgress` CAS 가 이미 동시 posting 을 막으므로 retain 경합 없음.

---

## 8. Stress Reproducer 스펙 (Q10)

| 항목 | 값 |
|------|-----|
| 동시 연결 수 | **10,000** |
| 실행 시간 | **5분** |
| 재연결 빈도 | 초당 100 connection churn (disconnect + 즉시 reconnect), 5분간 30k churn |
| 서버 모드 | IOCP, RIO 각각 실행 |
| 검증 | UAF 크래시 0회, `_CrtDbgReport` leak 0, 세션 destruct = accept 로그 일치 |
| 환경 | 로컬 Windows 11 Pro, x64 Debug + Release 양쪽 |
| 도구 | FastPortTestClient Scale 탭 확장 or 전용 CLI 모드 (Design 에서 결정) |

---

## 9. Next Steps

1. [ ] `/pdca design iosession-lifetime-race` — Design 문서 (retain/reset table, exit-path 매핑, RAII helper 여부, RIO 세부 구조)
2. [ ] `/pdca do iosession-lifetime-race --scope reproducer` — Stress 재현 도구 먼저 (Q4: Release 재현 선행)
3. [ ] `/pdca do iosession-lifetime-race --scope iocp` — IOCP 패치
4. [ ] `/pdca do iosession-lifetime-race --scope rio` — RIO 패치
5. [ ] `/pdca do iosession-lifetime-race --scope tests` — L1 lifetime 테스트
6. [ ] `/pdca analyze iosession-lifetime-race`
7. [ ] `/pdca report iosession-lifetime-race`

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-04-21 | Initial draft (Q1~Q10 결정 반영) | AnYounggun |
