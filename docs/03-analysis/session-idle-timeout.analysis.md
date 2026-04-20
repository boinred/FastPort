# session-idle-timeout Gap Analysis

> **Feature**: session-idle-timeout
> **Date**: 2026-04-20
> **Phase**: Check
> **Author**: AnYounggun

---

## Context Anchor

| Key | Value |
|-----|-------|
| **WHY** | 비정상 단절·freeze 미감지로 유령 세션, 상태 불일치, 릭 위험 |
| **WHO** | FastPort 엔진 서버 운영자, 매칭 로직 개발자 |
| **RISK** | 세션 맵 race, 오탐, 타이머-워커 동기화, timeout 폭주 |
| **SUCCESS** | 감지 ≤ 11s, 오탐 0, 릭 0, IOCP 먼저 적용 후 RIO 확장 |
| **SCOPE** | Phase 1 IOCP + LibNetworks 공용. RIO / Ping-Pong 은 후속 |

---

## 1. Strategic Alignment

### 1.1 PRD 핵심 문제 해결

| 항목 | 상태 | 근거 |
|---|:-:|---|
| 비정상 단절 수초 내 감지 | ✅ | `threshold(10s) + tick(1s)` = 최대 11s 이내 감지 (IdleChecker 로직) |
| 유령 세션 제거 | ✅ | Idle 시 `RequestDisconnect(IdleTimeout)` → 기존 OnDisconnected 경로로 SessionContainer 에서 제거 |
| Keep-Alive 대체 | ✅ | 애플리케이션 레벨 감지로 OS 30s+ 의존 제거 |
| `global-timer-queue` 기반 활용 | ✅ | `TimerQueue::GetInstance().SchedulePeriodic` 사용 |

### 1.2 Key Design 결정 준수

| 결정 | 구현 상태 |
|---|:-:|
| Option B (Clean): IIdleAware + non-template Checker | ✅ |
| Container ForEach/Snapshot | ✅ |
| SnapshotProvider 콜백 기반 타입 불변 | ✅ |
| RequestDisconnect(reason) 오버로드 | ✅ |
| `OnDisconnected` 시그니처 유지 | ✅ |
| TimerQueue 싱글톤 | ✅ |
| IdleCheckerConfig 생성자 주입 | ✅ |
| Phase 2(Ping/Pong) 범위 제외 | ✅ |
| RIO 범위 제외 | ✅ (사용자 지시 반영) |

**결론**: 전략적 일탈 없음. 모든 Key Design 결정 준수.

---

## 2. Plan Success Criteria

### 2.1 Functional Requirements (10/10 Met)

| ID | 요구사항 | 상태 | 근거 |
|----|----------|:---:|---|
| FR-01 | `m_LastRecvTimeMs` atomic 추가 + 수신 경로 갱신 | ✅ Met | `IOSession.ixx:~145` + `IOSession.cpp` Real Recv 성공 경로 |
| FR-02 | LibNetworks 공용 Idle Checker 헬퍼 | ✅ Met | `SessionIdleChecker.ixx/cpp` (non-template, SnapshotProvider) |
| FR-03 | `IdleCheckerConfig` 구조체 | ✅ Met | `SessionIdleChecker.ixx:29` (threshold/tick/enabled) |
| FR-04 | `SchedulePeriodic` + Cancel | ✅ Met | `SessionIdleChecker::Start/Stop` |
| FR-05 | IOCP 서버 설치 | ✅ Met | `IOCPServiceMode.cpp` OnStarted/OnStopped/OnShutdown |
| FR-06 | `RequestDisconnect(IdleTimeout)` | ✅ Met | OnTick 에서 임계 초과 세션에 호출 |
| FR-07 | `RequestDisconnect(reason)` 오버로드 | ✅ Met | `IOSession::RequestDisconnect(DisconnectReason)` public override |
| FR-08 | Idle disconnect 로그 | ✅ Met | "IdleTimeout detected. Session Id : N, IdleMs : M" (`IOSession.cpp`) |
| FR-09 | 기본값 10s/1s/enabled | ✅ Met | `IdleCheckerConfig` 기본값 + IOCPServiceMode cfg |
| FR-10 | Shutdown 안전 종료 | ✅ Met | `TimerQueue::Cancel(wait=true)` → 진행 중 tick 완료 대기 |

**FR 충족률**: **10/10 (100%)**

### 2.2 Non-Functional Requirements

| Category | 기준 | 상태 | 근거 |
|---|---|:-:|---|
| Detection latency | ≤ threshold + tick | ✅ Met | I-03 테스트 500ms 대기 내 stale 감지 확인 |
| False positive | 정상 세션 오탐 0 | ✅ Met | I-02 (30ms 주기 갱신) + I-04 (fresh 세션) |
| Performance | 1만 세션 CPU<1% | ⚠️ Not Measured | 스트레스 테스트 미수행 — Plan V-1 과 동일하게 후속 최적화 |
| Thread safety | 타이머-워커 race 없음 | ✅ Met | relaxed atomic, snapshot 기반, per-session catch-all |
| Memory safety | 릭 없음 | ⚠️ Not Measured | `_CrtDumpMemoryLeaks` 미수행 |
| Build | MSVC 2022 x64 Debug | ✅ Met | 솔루션 전체 빌드 OK |

**NFR 충족**: 4/6 Met + 2/6 미측정 (Non-Critical, 후속 스코프로 분리 가능)

---

## 3. Structural Match

### 3.1 파일 존재 / 프로젝트 등록

| Design §11.1 | 실제 | 매치 |
|---|---|:-:|
| `LibCommons/Container.ixx` (수정) | ForEach + Snapshot 추가 | ✅ |
| `LibNetworks/INetworkSession.ixx` (수정) | DisconnectReason enum | ✅ |
| `LibNetworks/IIdleAware.ixx` (신규) | 존재, 등록됨 | ✅ |
| `LibNetworks/IOSession.{ixx,cpp}` (수정) | IIdleAware 상속, m_LastRecvTimeMs, RequestDisconnect(reason) | ✅ |
| `LibNetworks/SessionIdleChecker.{ixx,cpp}` (신규) | 존재, 등록됨 | ✅ |
| `FastPortServer/IOCPServiceMode.{ixx,cpp}` (수정) | m_IdleChecker 연동 | ✅ |
| `LibCommonsTests/ContainerTests.cpp` (신규) | 5 테스트 | ✅ |
| `LibNetworksTests/SessionIdleCheckerTests.cpp` (신규) | 8 테스트 | ✅ |
| L2 IOSession lastRecv 통합 테스트 | ❌ 미구현 | 차감 |

**Structural Match**: **95%** (L2 통합 테스트 미구현)

---

## 4. Functional Depth

### 4.1 Logic 완성도

| 영역 | 상태 |
|---|:-:|
| Container ForEach/Snapshot — read-lock 유지, 복사 | ✅ 완전 |
| IOSession::OnIOCompleted — Real Recv(bytes>0+CommitWrite성공) 시 갱신 | ✅ |
| RequestDisconnect(reason) — IdleTimeout 시 idle duration 로그 | ✅ |
| SessionIdleChecker::Start — enabled/중복호출 체크 | ✅ |
| OnTick — late check, provider catch-all, per-session catch-all, lastRecv=0 skip | ✅ |
| Stop — m_Running=false + TimerQueue.Cancel(wait) 순서 | ✅ |
| IOCPServiceMode — SnapshotProvider static_pointer_cast 안전 | ✅ |

**Placeholder/Stub 없음**. 모든 경로 실동작.

### 4.2 Design §6.1 예외 시나리오 커버

| # | Design 상황 | 구현 상태 |
|---|---|:-:|
| 1 | enabled=false → no-op | ✅ I-01 |
| 2 | 이중 Start | ✅ m_Running CAS |
| 3 | 빈 vector | ✅ loop no-iter |
| 4 | Provider 예외 | ✅ I-07 |
| 5 | lastRecv=0 skip | ✅ I-05 |
| 6 | RequestDisconnect 예외 | ✅ I-08 |
| 7 | Stop 중복 | ✅ wasRunning 체크 |
| 8 | dtor 안전 | ✅ Stop() 호출 |

**Functional Depth**: **100%**

---

## 5. API Contract Match

### 5.1 Design §4 vs 실제

| Design 선언 | 구현 | 일치 |
|---|---|:-:|
| `DisconnectReason` enum (5 값) | `INetworkSession.ixx` 그대로 | ✅ |
| `IIdleAware { GetLastRecvTimeMs, RequestDisconnect(reason) }` | `IIdleAware.ixx` 그대로 | ✅ |
| `IdleCheckerConfig { thresholdMs, tickIntervalMs, enabled }` | 그대로 | ✅ |
| `SessionIdleChecker(cfg, provider)` + Start/Stop | 그대로, + `GetDisconnectCount()` 보너스 | ✅ (확장) |
| `SnapshotProvider = function<vector<shared_ptr<IIdleAware>>()>` | 그대로 | ✅ |
| `Container::ForEach(fn) / Snapshot()` | 그대로 + 콜백 mutation 금지 주석 | ✅ |
| `IOSession : IIdleAware` + public `RequestDisconnect(reason)` | 그대로 | ✅ |

**API Contract Match**: **100%**

---

## 6. Runtime Verification

### 6.1 단위 테스트 실행 결과

| Suite | 결과 |
|---|---|
| **ContainerTests** (C-01~C-05) | **5/5 Pass** |
| **SessionIdleCheckerTests** (I-01~I-08) | **8/8 Pass** |
| 기존 TimerQueueTests (선행 피처) | **19/19 Pass** |
| 기존 CircleBufferQueueTests 등 | **정상** |
| **소계** | **13/13 신규 + 기존 전부 유지** |

### 6.2 미수행 (차감)

| 항목 | 사유 |
|---|---|
| L2 IOSession lastRecv 통합 테스트 (3개 Design §8.4) | 실제 소켓 loopback 필요, 다음 스코프 or 후속 테스트 사이클 |
| L3 수동 시나리오 (FastPortServer + TestClient 강제 종료) | 상호작용 작업, 사용자 직접 검증 영역 |

**Runtime Score**:
- 수행된 L1 단위 테스트 전부 Pass
- L2/L3 미수행 감점 → **85%** 수준

---

## 7. Decision Record Verification

| 결정 | 준수 | 비고 |
|---|:-:|---|
| [PRD] 1차 소비자 = session-idle-timeout | ✅ | 구현됨 (이 피처) |
| [Plan] Q1 SessionContainer 재사용 (B안) | ✅ | `IOCPInboundSession` 의 SingleTon 공유 |
| [Plan] Q2 OnIOCompleted 갱신 | ✅ | Real Recv 경로 |
| [Plan] Q3 RequestDisconnect(reason) 오버로드 | ✅ | 시그니처 변경 없이 오버로드 추가 |
| [Plan] Q4 TimerQueue 싱글톤 | ✅ | `GetInstance()` |
| [Plan] Q5 생성자 파라미터 주입 | ✅ | `IdleCheckerConfig` |
| [Plan] Q6 Ping/Pong out-of-scope | ✅ | Future Work 만 |
| [Design] Option B (Clean) | ✅ | IIdleAware 인터페이스 |
| [Design] RIO 제외 (사용자 지시) | ✅ | IOCPServiceMode 만 연동 |

**Decision 준수율**: **9/9 (100%)**

---

## 8. Match Rate 계산

### 8.1 축별 점수

| 축 | 가중치 | 점수 | 기여 |
|---|:-:|:-:|:-:|
| Structural | 0.15 | 95% | 14.25 |
| Functional Depth | 0.25 | 100% | 25.00 |
| API Contract | 0.25 | 100% | 25.00 |
| Runtime | 0.35 | 85% | 29.75 |

### 8.2 Overall

**Overall = 14.25 + 25.00 + 25.00 + 29.75 = 94.00%**

---

## 9. Gap List

### 9.1 Critical

**없음**

### 9.2 Important

| # | 영역 | 현재 | 권장 조치 |
|---|---|---|---|
| I-1 | L3 수동 시나리오 미검증 | 서버 실행 + TestClient 강제 종료 테스트 미수행 | Report 전에 수동 검증 1회 권장 (직접 실행) |

### 9.3 Minor (Non-Critical 검증 공백)

| # | 영역 | 사유 |
|---|---|---|
| M-1 | L2 IOSession lastRecv 통합 테스트 | Mock 없이 실제 소켓 loopback 필요 — 후속 스코프 |
| M-2 | 1만 세션 스트레스 | Plan NFR 측정 공백 |
| M-3 | 메모리 릭 자동 검증 | `_CrtDumpMemoryLeaks` 미수행 |

---

## 10. 결론 및 권고

**Match Rate: 94.00%** → **Report 단계 진행 가능** (기준 ≥90% 충족).

- Critical 이슈: **없음**
- 모든 FR (10/10) 및 Key Design 결정 (9/9) 충족
- 13개 단위 테스트 모두 통과 (Container 5 + SessionIdleChecker 8)
- L3 수동 시나리오 + L2 통합 테스트만 미수행 (Non-Critical)

### 10.1 선택지

1. **바로 Report**: `/pdca report session-idle-timeout` — 현재 상태로 마무리
2. **L3 수동 검증 후 Report**: FastPortServer 실행 + TestClient 강제 종료로 실제 시나리오 확인
3. **후속 최적화 스코프**: 성능 측정 / 릭 검증을 별도 사이클로 (global-timer-queue 와 유사하게 분리)

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-04-20 | Initial gap analysis (Match Rate 94.00%) | AnYounggun |
