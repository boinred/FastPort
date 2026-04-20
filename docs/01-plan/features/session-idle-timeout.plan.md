# session-idle-timeout Planning Document

> **Summary**: 세션별 `lastRecvTime` 추적 + `LibCommons::TimerQueue` tick 기반 애플리케이션 idle timeout. 비정상 단절 시 서버가 수 초 내 감지하여 유령 세션 제거.
>
> **Project**: FastPort
> **Author**: AnYounggun
> **Date**: 2026-04-20
> **Status**: Draft

---

## Executive Summary

| Perspective | Content |
|-------------|---------|
| **Problem** | 클라이언트 비정상 단절(프로세스 강제종료·네트워크 차단·freeze) 시 서버가 TCP Keep-Alive(30초+) 에만 의존하여 유령 세션 누적, freeze 의 경우 감지 불가 |
| **Solution** | `IOSession` 베이스에 `atomic<int64_t> m_LastRecvTimeMs` 추가 + `TimerQueue` 주기 tick 으로 idle 검사 → `RequestDisconnect(reason=IdleTimeout)` 호출. 공통 idle check 로직은 `LibNetworks` 템플릿 헬퍼로 제공 |
| **Function/UX Effect** | 비정상 단절 시 `idleThresholdMs + tickIntervalMs` 이내(기본 ~11초) 감지. 기존 `OnDisconnected` 경로로 정리 동작 모두 재사용 |
| **Core Value** | 유령 세션 제거로 리소스 안정성 확보, 매칭/파티 로직 상태 정합성 향상, 후속 Ping/Pong 하트비트(Phase 2) 기반 |

---

## Context Anchor

| Key | Value |
|-----|-------|
| **WHY** | 비정상 단절·freeze 미감지로 유령 세션 축적, 상태 불일치, RIO 버퍼 릭 위험 |
| **WHO** | FastPort 엔진 서버 운영자 / 매칭 로직 개발자 / 클라이언트 개발자 |
| **RISK** | 콜백에서 세션 맵 순회 중 race, 정상 트래픽 오탐, 타이머-워커 간 동기화, 대량 동시 timeout 부하 스파이크 |
| **SUCCESS** | 비정상 단절 감지 ≤ 11s(기본), IOCP 구현 후 동일 패턴 RIO 적용 가능, 정상 세션 오탐 0, 리소스 릭 0 |
| **SCOPE** | Phase 1 — FastPortServer(IOCP) 에 idle timeout 적용 + LibNetworks 공용 헬퍼. Phase 2 — Ping/Pong(Future Work). RIO 적용은 후속 |

---

## 1. Overview

### 1.1 Purpose

애플리케이션 레벨 idle timeout 으로 비정상 단절을 능동 감지하여 유령 세션을 제거한다. OS 레벨 TCP Keep-Alive(30s+) 대비 수 초 이내 감지 가능하게 만드는 것이 목표.

### 1.2 Background

- **현재 단절 감지 구조** (IOSession.cpp 참조):
  - graceful close: `recv` 0 byte → `RequestDisconnect()` (즉시)
  - 비정상 단절(케이블 분리/freeze): TCP Keep-Alive probe 실패까지 **30초 이상**
  - 클라이언트 freeze(살아있지만 미송신): **감지 불가**
- **유사 현상의 과거 비용**: RIO 안정성 수정 5건(커밋 `72d53cd`)처럼 세션 수명 처리 이슈는 재발 위험이 크고 디버깅 비용 높음
- **인프라 준비 완료**: 선행 피처 `global-timer-queue` 로 `LibCommons::TimerQueue` 사용 가능

### 1.3 Related Documents

- PRD: `docs/00-pm/session-idle-timeout.prd.md`
- 선행 피처 Report: `docs/04-report/global-timer-queue.report.md`
- 참고 구현: `LibCommons/TimerQueue.ixx` (주기 tick 기반)

---

## 2. Scope

### 2.1 In Scope (Phase 1)

- [ ] `IOSession` 베이스에 `std::atomic<int64_t> m_LastRecvTimeMs` 멤버 추가
- [ ] 생성/Accept 시점에 초기값 기록, 수신 경로(`OnIOCompleted`, 바이트 > 0) 에서 갱신
- [ ] `LibNetworks` 에 공통 Idle Checker 템플릿 헬퍼 추가
  - 세션 컨테이너 타입을 템플릿 파라미터로 받음 (IOCP/RIO 둘 다 사용 가능한 형태)
  - 이번 스코프는 IOCP 용 소비만 구현
- [ ] `IdleCheckerConfig { thresholdMs, tickIntervalMs, enabled }` 구조체 + 생성자 파라미터
- [ ] `FastPortServer`(IOCP) 에 idle checker 설치 — `IOCPServiceMode::OnStarted` 에서 시작, `OnStopped/OnShutdown` 에서 정리
- [ ] `RequestDisconnect(DisconnectReason)` 오버로드 추가 (기본 호출은 `Normal`, idle 경로는 `IdleTimeout`)
- [ ] `OnDisconnected()` 시그니처는 유지(호환성), 사유는 로그로 기록
- [ ] 단위 테스트:
  - 정상 트래픽 흐르는 세션은 절대 오탐 없음
  - 수신 끊긴 세션은 `thresholdMs + tickIntervalMs` 이내 `RequestDisconnect` 호출 확인
  - 대량 세션(100+) 에서 동시 timeout 처리 안전성
- [ ] Logger 연동 (`"IdleChecker"` 카테고리)

### 2.2 Out of Scope

- **RIO 적용** — 동일 패턴이지만 별도 스코프(RIO 프로젝트 재오픈 시). Phase 1 완료 후 1-2세션으로 적용 가능
- **Ping/Pong 하트비트 프로토콜** — Phase 2. 프로토콜 변경(`Protos`) + 클라이언트/테스트클라이언트 동시 수정 필요
- **세션별 차등 타임아웃** (인증 전 짧게, 인증 후 길게) — 확장 포인트만 열어두기
- **다른 종류 타임아웃** (핸드셰이크/로그인/요청별) — 별도 피처
- **disconnect reason 세분화/전파** — `OnDisconnected` 시그니처 변경 안 함, 로그로만 기록

---

## 3. Requirements

### 3.1 Functional Requirements

| ID | Requirement | Priority | Status |
|----|-------------|----------|--------|
| FR-01 | `IOSession::m_LastRecvTimeMs` 원자적 멤버 추가 및 수신 경로에서 갱신 | High | Pending |
| FR-02 | `LibNetworks` 공통 Idle Checker 템플릿 헬퍼 제공 | High | Pending |
| FR-03 | `IdleCheckerConfig` 구조체 (thresholdMs / tickIntervalMs / enabled) | High | Pending |
| FR-04 | `TimerQueue::GetInstance().SchedulePeriodic` 으로 tick 등록, 종료 시 Cancel | High | Pending |
| FR-05 | IOCP 서버(`FastPortServer`) 에 idle checker 설치 | High | Pending |
| FR-06 | 임계 초과 세션에 대해 `RequestDisconnect(IdleTimeout)` 호출 | High | Pending |
| FR-07 | `RequestDisconnect(reason)` 오버로드 추가, 기본값 `Normal` | Medium | Pending |
| FR-08 | Idle disconnect 시 로그 출력 (`"IdleChecker"` 카테고리, 세션 id / idle duration / threshold) | Medium | Pending |
| FR-09 | 설정 기본값: `thresholdMs=10000`, `tickIntervalMs=1000`, `enabled=true` | Medium | Pending |
| FR-10 | Checker Shutdown 시 현재 진행 중인 tick 콜백 완료 대기 후 안전 종료 (TimerQueue Cancel 의 Wait path 재사용) | High | Pending |

### 3.2 Non-Functional Requirements

| Category | Criteria | Measurement Method |
|----------|----------|-------------------|
| **Detection latency** | 정상적 비정상 단절 감지 ≤ `thresholdMs + tickIntervalMs` (기본 11s) | 단위 테스트에서 `steady_clock` 측정 |
| **False positive** | 정상 트래픽(1초 이하 주기 송신) 세션은 절대 오탐 없음 | 트래픽 주입 테스트 |
| **Performance** | 1만 세션 기준 tick 1회 처리 CPU < 1% | 향후 벤치마크(후속 스코프) |
| **Thread safety** | 타이머 콜백과 워커 간 race 없음 (수신 업데이트 vs 검사 순회) | atomic 단일 정수 읽기/쓰기 |
| **Memory safety** | idle disconnect 경로도 기존 `RequestDisconnect` 와 동등한 정리 (m_DisconnectRequested CAS 재사용) | 기존 테스트 + 수동 릭 확인 |
| **Build** | MSVC 2022 x64 Debug/Release | `FastPort.slnx` 빌드 성공 |

---

## 4. Success Criteria

### 4.1 Definition of Done

- [ ] FR-01 ~ FR-10 모두 구현
- [ ] 단위 테스트 작성:
  - [ ] `Idle_NormalTraffic_NoFalsePositive` — 100ms 주기로 수신 → 500ms threshold 동안 유지 → disconnect 호출 없음
  - [ ] `Idle_StaleSession_DisconnectsWithinBudget` — 수신 중단 → threshold + tick 이내 disconnect 요청 확인
  - [ ] `Idle_ConfigDisabled_NeverDisconnects` — `enabled=false` 면 tick 자체가 등록되지 않음
  - [ ] `Idle_MultipleSessions_EachIndependent` — 여러 세션, 일부만 stale → 정확히 그 세션만 disconnect
  - [ ] `Idle_ShutdownDuringTick_NoCrash` — checker shutdown 시 진행 중 tick 안전 종료
- [ ] `FastPortServer.exe` 실행 후 수동 시나리오 통과:
  - [ ] TestClient 로 연결 → 송수신 활발 → 끊기지 않음
  - [ ] TestClient 강제 종료(Task Manager Kill) → 약 11s 내 서버 로그에 "IdleTimeout" 출력 + 세션 제거
- [ ] 빌드 warning 0
- [ ] Logger 출력이 `loggers/log_*.txt` 파일과 콘솔 양쪽 반영

### 4.2 Quality Criteria

- [ ] 단위 테스트 전부 pass (신규 ≥ 5건)
- [ ] MSVC Level3 경고 0
- [ ] `.clang-format` 준수 (프로젝트 스타일)
- [ ] `_CrtDumpMemoryLeaks` 또는 수동 릭 확인

---

## 5. Risks and Mitigation

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| **정상 세션 오탐**: `thresholdMs` 너무 짧으면 느린 네트워크 환경에서 끊김 | High | Low | 기본값 10s 는 보수적. 문서에 조정 가이드 포함. 단위 테스트에서 오탐 0 강제 |
| **타이머 콜백 실행 중 컨테이너 수정(추가/제거) race** | High | Medium | 콜백에서는 snapshot 취한 뒤 순회 (기존 `SessionContainer` 의 스레드 안전 API 가정 — 확인 필요. 없으면 ID 목록만 스냅샷 후 개별 조회) |
| **`RequestDisconnect` 재진입 / 이중 호출** | Medium | Medium | 기존 `m_DisconnectRequested` atomic CAS 가 이미 처리. idle 경로도 동일 CAS 사용 |
| **Shutdown 중 tick 이 해제된 컨테이너 접근** | Critical | Low | checker destructor 에서 `Cancel(id, waitForCallbacks=true)` → 진행 중 콜백 완료 대기 후 해제 |
| **대량 동시 timeout → disconnect 폭주로 부하 스파이크** | Medium | Low | tick 당 최대 N개 제한 옵션(확장 포인트). 현재 기본값에선 1만 세션 동시 timeout 가정 드물다고 판단 |
| **`SessionContainer` 의 스레드 안전성 미보장** | High | Medium | `LibCommons/Container.ixx` 내부 확인 필요. 필요 시 shared_mutex 로 감싸거나 read 전용 snapshot API 사용 |
| **`std::chrono::steady_clock` 오차 / wrap** | Low | Low | `int64_t` ms 단위 저장. 99999년 분량. 사실상 무시 가능 |
| **Logger 가 모듈 구현 단위에서 ICE** | Medium | Low | 선행 피처 학습 적용: GMF 에 `#include <spdlog/spdlog.h>` 포함 (`CLAUDE.md` 지침) |

---

## 6. Impact Analysis

### 6.1 Changed Resources

| Resource | Type | Change Description |
|----------|------|--------------------|
| `LibNetworks/IOSession.ixx` | Header modification | `m_LastRecvTimeMs` atomic 멤버 추가, `GetLastRecvTimeMs()` 접근자, `RequestDisconnect(DisconnectReason)` 오버로드 선언 |
| `LibNetworks/IOSession.cpp` | Impl | `OnIOCompleted` 수신 경로에 time 갱신 로직 추가, `RequestDisconnect(reason)` 구현 |
| `LibNetworks/INetworkSession.ixx` | Interface | `GetLastRecvTimeMs()` virtual 추가 (공통 접근용), `enum class DisconnectReason { Normal, IdleTimeout, Backpressure, ... }` 정의 |
| `LibNetworks/SessionIdleChecker.ixx` | New Module | 템플릿 헬퍼 (`template <typename SessionT>`). Config + Start/Stop API + tick 콜백 |
| `LibNetworks/SessionIdleChecker.cpp` | New Module Impl | TimerQueue 연동, 세션 순회 로직 |
| `LibNetworks/LibNetworks.vcxproj(.filters)` | Project files | 신규 파일 등록 |
| `FastPortServer/IOCPServiceMode.ixx` | Modification | `m_IdleChecker` 멤버 추가 |
| `FastPortServer/IOCPServiceMode.cpp` | Modification | `OnStarted` 에서 checker 생성/시작, `OnStopped/OnShutdown` 에서 정리 |
| `LibNetworksTests/SessionIdleCheckerTests.cpp` | New Test | FR/NFR 검증용 테스트 5건+ |

### 6.2 Current Consumers

**이번 피처는 신규 기능이며 외부 공개 API 를 크게 바꾸지 않음.** 영향 범위:

| Resource | Operation | Code Path | Impact |
|----------|-----------|-----------|--------|
| `IOSession` | 상속 | `InboundSession`, `OutboundSession`, `IOCPInboundSession`, `IOCPOutboundSession` | 멤버 추가만 — 기존 동작 유지 |
| `RequestDisconnect()` | 호출 | 다수 경로(`IOSession.cpp` 의 8곳) | 기본값 reason=Normal 이라 호환 |
| `OnDisconnected()` | 시그니처 | 전체 세션 계층 | **시그니처 유지** — 영향 없음 |
| `LibCommons::TimerQueue` | Schedule/Cancel | 신규 의존 | 선행 피처에서 이미 안정화 |

### 6.3 Verification

- [ ] `InboundSession`/`OutboundSession` 은 기존 멤버 사용 방식 그대로 작동
- [ ] 기존 테스트 (`LibNetworksTests`) 전부 pass
- [ ] IOCP 서버 실행 후 TestClient 연결/송수신 정상
- [ ] `RequestDisconnect()` 호출점 8곳 모두 컴파일 유지 (기본값 reason)

---

## 7. Architecture Considerations

### 7.1 Project Level Selection

| Level | Characteristics | Recommended For | Selected |
|-------|-----------------|-----------------|:--------:|
| **Starter** | 단순 구조 | 정적 사이트 | ☐ |
| **Dynamic** | Feature 모듈 | SaaS | ☐ |
| **Enterprise** | 계층 분리, 고성능 | 게임/네트워크 엔진 | ☑ |

### 7.2 Key Architectural Decisions

| Decision | Options | Selected | Rationale |
|----------|---------|----------|-----------|
| 세션 등록소 | 신규 Registry / 기존 SessionContainer 재사용 / 공통 베이스 통일 | **기존 SessionContainer 재사용** | 이미 IOCP 프로젝트에서 운용 중 (IOCPInboundSession.cpp:52). 새 등록소는 중복 |
| Idle Checker 위치 | 프로젝트별 / **LibNetworks 공용 템플릿** / 공통 베이스 Registry | **LibNetworks 공용 템플릿** | 중복 제거. IOCP/RIO 모두 재사용. 타입 안전성 유지 |
| `lastRecvTime` 업데이트 지점 | OnIOCompleted / OnPacketReceived | **OnIOCompleted (바이트 > 0)** | 네트워크 바이트 수신 = 생존 증거. 부분 패킷 수신도 반영 |
| Disconnect 사유 전파 | 시그니처 유지 로그만 / OnDisconnected(reason) 변경 / **RequestDisconnect(reason) 오버로드** | **RequestDisconnect(reason) 오버로드** | 소비자 영향 최소. 후속 확장 여지 |
| TimerQueue 인스턴스 | 싱글톤 GetInstance / 소유 인스턴스 | **싱글톤** | TimerQueue 는 전역 유틸로 설계 |
| Config 주입 | 빌드 상수 / 생성자 파라미터 / 외부 파일 | **생성자 파라미터** | 단순, 테스트 용이, YAGNI 관점에서 JSON 설정 불필요 |
| Logger | spdlog 직접 / LibCommons::Logger | **LibCommons::Logger** | 프로젝트 표준 (CLAUDE.md 지침), GMF 에 spdlog 포함 |
| Ping/Pong | 포함 / 분리 | **분리 (Phase 2)** | 프로토콜 변경 + client/testclient 동시 수정 필요 → 범위 폭증 |

### 7.3 Clean Architecture

```
Selected Level: Enterprise (C++ native)

Layer:
┌─────────────────────────────────────────────────────────────┐
│ Consumer Layer (FastPortServer/IOCPServiceMode)            │
│   ↓                                                         │
│ Infrastructure Layer (LibNetworks/SessionIdleChecker)      │
│   ↓ uses                                                    │
│ ┌───────────────────────────┐  ┌──────────────────────────┐│
│ │ LibCommons::TimerQueue    │  │ LibNetworks::IOSession   ││
│ │ (commons.timer_queue)     │  │ (m_LastRecvTimeMs atomic)││
│ └───────────────────────────┘  └──────────────────────────┘│
│            ↓                                ↓              │
│ Existing SessionContainer (SingleTon<Container<...>>)      │
└─────────────────────────────────────────────────────────────┘

Module Names:
- LibNetworks/SessionIdleChecker.ixx  → `networks.sessions.idle_checker`
```

### 7.4 API Sketch

```cpp
// LibNetworks/INetworkSession.ixx (확장)
export enum class DisconnectReason : std::uint8_t {
    Normal        = 0,
    IdleTimeout   = 1,
    Backpressure  = 2,
    Protocol      = 3,
    Server        = 4,
};

// LibNetworks/IOSession.ixx (확장)
export class IOSession : public ... {
public:
    std::int64_t GetLastRecvTimeMs() const noexcept {
        return m_LastRecvTimeMs.load(std::memory_order_relaxed);
    }

    void RequestDisconnect();                        // 기존 (reason = Normal)
    void RequestDisconnect(DisconnectReason reason); // 신규 오버로드

protected:
    std::atomic<std::int64_t> m_LastRecvTimeMs { 0 };
    // OnIOCompleted 수신 경로에서 갱신:
    //   m_LastRecvTimeMs.store(NowMs(), memory_order_relaxed);
};

// LibNetworks/SessionIdleChecker.ixx (신규)
export module networks.sessions.idle_checker;

export struct IdleCheckerConfig {
    std::chrono::milliseconds thresholdMs   { 10000 };
    std::chrono::milliseconds tickIntervalMs{ 1000 };
    bool                      enabled       { true };
};

export template <typename SessionT>
class SessionIdleChecker
{
public:
    // SessionT 는 GetSessionId(), GetLastRecvTimeMs(), RequestDisconnect(DisconnectReason) 제공 필요
    explicit SessionIdleChecker(const IdleCheckerConfig& cfg);
    ~SessionIdleChecker();

    void Start();
    void Stop();

    IdleCheckerConfig GetConfig() const { return m_Config; }

private:
    void OnTick();

    IdleCheckerConfig              m_Config;
    LibCommons::TimerId            m_TimerId = LibCommons::kInvalidTimerId;
    std::atomic<bool>              m_Running{ false };
};
```

소비자 사용 예:

```cpp
// FastPortServer/IOCPServiceMode.cpp (OnStarted 내부)
using IdleChecker = LibNetworks::SessionIdleChecker<
    LibNetworks::Sessions::InboundSession
>;
m_IdleChecker = std::make_shared<IdleChecker>(
    IdleCheckerConfig{ 10000ms, 1000ms, true });
m_IdleChecker->Start();

// OnStopped / OnShutdown
m_IdleChecker->Stop();
```

---

## 8. Convention Prerequisites

### 8.1 Existing Project Conventions

- [x] `CLAUDE.md` 프로젝트 루트 — 로깅·타이머 콜백 수명·C++ 표준·네이밍 지침 존재 (선행 피처에서 업데이트됨)
- [x] 통일된 모듈 네이밍 `networks.sessions.xxx`
- [x] `std::atomic<int64_t>` + `m_` 접두 멤버 스타일
- [x] `LibCommons::Logger` 표준 사용

### 8.2 Conventions to Define/Verify

| Category | Current | To Define | Priority |
|----------|---------|-----------|:--------:|
| **DisconnectReason enum** | 부재 | `INetworkSession.ixx` 에 정의 | High |
| **Time utility** | 각 cpp 에서 `high_resolution_clock` 사용 | `steady_clock` + helper `NowMs()` 통일 제안 | Medium |
| **Idle 로깅 카테고리** | 없음 | `"IdleChecker"` 표준 카테고리 | High |
| **Container 스레드 안전성** | 미확인 | `LibCommons/Container.ixx` 내부 확인 | High |

### 8.3 Environment Variables

불필요. 모든 설정은 생성자 파라미터.

---

## 9. Next Steps

1. [ ] Design 문서 (`/pdca design session-idle-timeout`)
   - 3가지 옵션 비교 (A: 최소 변경 / B: Clean / C: Pragmatic)
   - `SessionContainer` 의 스레드 안전성 내부 확인 결과 반영
   - `SessionIdleChecker` 내부 설계 (tick 콜백 순회 전략 상세)
2. [ ] Do 구현 (IOCP 범위)
3. [ ] Check — 단위 테스트 + 수동 시나리오
4. [ ] Report
5. [ ] (후속) RIO 적용 스코프 재개

---

## 10. Future Work (Phase 2)

- **Ping/Pong 하트비트 프로토콜**: `Protos` 에 `Ping`/`Pong` 메시지 추가, 클라이언트가 주기 Ping, 서버가 Pong 응답. `thresholdMs` 를 더 짧게(예: 5s) 낮출 수 있음
- **세션별 차등 threshold**: 인증 전(짧게)/인증 후(길게) 구분
- **RIO 서버 적용**: 동일 `SessionIdleChecker` 템플릿 사용
- **Metrics/Observability**: idle disconnect 카운터, 평균 idle duration 기록
- **Backpressure 결합**: `lastRecvTime` 외에 send queue 깊이 기반 감지

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-04-20 | Initial draft (RIO 는 out-of-scope, Q1=b LibNetworks 공용 템플릿 확정) | AnYounggun |
