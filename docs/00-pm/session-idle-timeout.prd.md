# session-idle-timeout - Product Requirements Document

> **Date**: 2026-04-17
> **Author**: AnYounggun
> **Method**: bkit PM Analysis (manual, Agent Teams unavailable)
> **Status**: Draft

---

## Executive Summary

| Perspective | Content |
|-------------|---------|
| **Problem** | 클라이언트가 비정상 단절(프로세스 강제 종료, 네트워크 차단, 케이블 분리)될 경우 서버가 즉시 인식하지 못해 유령 세션이 남고, TCP Keep-Alive(30s idle + probe)는 게임 서버 요구(수 초 이내)에 비해 너무 느리다 |
| **Solution** | 세션별 `lastRecvTime` 추적 + **TimerQueue 기반 idle timeout 감시**로 애플리케이션 레벨 비정상 단절 감지. 선택적으로 애플리케이션 하트비트 프로토콜 추가 |
| **Target User** | FastPort 엔진을 사용하는 게임 서버 개발자(본인 포함), 실운영 시 유령 세션으로 인한 리소스 누수/로직 오류를 겪는 팀 |
| **Core Value** | 비정상 단절을 1–10초 이내 감지, 유령 세션 제거로 리소스 안전성 확보, IOCP/RIO 양쪽 엔진 동등 동작 보장(behavioral parity) |

---

## 1. Opportunity Discovery

### 1.1 Current Behavior (As-Is)

| 단절 유형 | 현재 서버 인식 경로 | 감지 지연 |
|-----------|---------------------|-----------|
| `shutdown()` / `closesocket()` (graceful) | recv 0바이트 → `RequestDisconnect()` | 즉시 |
| `DisconnectEx` (graceful) | RIO/IOCP completion | 즉시 |
| 클라이언트 프로세스 강제 종료 (OS FIN 전송) | 대부분 recv 0바이트로 감지 | 즉시~수초 |
| **네트워크 단절 (케이블 분리, Wi-Fi off, 방화벽 drop)** | TCP Keep-Alive probe 실패 시 `WSAETIMEDOUT` | **~30초 이상** |
| **클라이언트 freeze / 무반응 (살아있지만 보내지 않음)** | **감지 불가** | **무한대** |

### 1.2 Problem Framing

현재 감지 구조는 **OS가 알려줄 때까지 기다리는 수동형**이다. 게임 서버는:
- 접속 슬롯/메모리/RIO 버퍼 풀 제약 → 유령 세션이 누적되면 실사용자 접속 차단
- 매칭/파티 로직에서 "이미 끊어진 유저"를 살아있다고 판단 → 상태 불일치
- RIO 버퍼 릭 위험 (이미 5건의 RIO 안정성 수정 이력 있음, 커밋 `72d53cd`)

### 1.3 Top Ideas (Brainstorm)

| # | Idea | 감지 속도 | 구현 복잡도 | 프로토콜 변경 |
|---|------|-----------|-------------|---------------|
| 1 | **Idle Timeout만** (수신 N초 없으면 끊음, TimerQueue) | 중간 (N초) | 낮음 | 없음 |
| 2 | **App-level Heartbeat Ping/Pong** + Idle Timeout | 빠름 (1–3초) | 중간 | **필요** (Protos 추가) |
| 3 | **TCP Keep-Alive 파라미터 강화** (idle 5s, interval 1s, count 3) | 중간 | 매우 낮음 | 없음 |
| 4 | Windows `SetWaitableTimer` per-session | 중간 | 높음 | 없음 |
| 5 | OS `RegisterWaitForSingleObject` + socket event | 중간 | 매우 높음 | 없음 |

### 1.4 Opportunity Solution Tree

```
Outcome: 비정상 단절을 빠르게 감지하고 유령 세션을 제거한다
├── Opportunity 1: 수동적 감지를 능동적 감지로 전환
│   ├── Solution A: TimerQueue + lastRecvTime idle check (아이디어 1)
│   └── Solution B: TCP Keep-Alive 공격적 튜닝 (아이디어 3)
├── Opportunity 2: 클라이언트 freeze/hang 감지
│   ├── Solution C: App Heartbeat Ping/Pong 프로토콜 (아이디어 2)
│   └── Solution D: 클라이언트가 주기적으로 빈 패킷 전송하는 규약
└── Opportunity 3: IOCP/RIO 엔진 간 동작 일관성
    └── Solution E: 공통 `ISessionTimeoutPolicy` 인터페이스 + 양쪽 구현
```

---

## 2. Recommended Approach

**Phase 1 (MVP)**: Solution A — **TimerQueue 기반 Idle Timeout**
- 세션별 `lastRecvTimeMs` 추적 (수신 경로에서 원자적 업데이트)
- 단일 `TimerQueue` (Windows `CreateTimerQueue`) + 주기 타이머(예: 1초)
- 타이머 콜백: 모든 active 세션 순회 → `now - lastRecv > idleThresholdMs` 이면 `RequestDisconnect()`
- IOCP/RIO 공통 `IOSession` 베이스 레벨에 구현
- 기본값: `idleThresholdMs = 10000` (10초), 설정 가능

**Phase 2 (옵션)**: Solution C — **Ping/Pong 하트비트**
- Protos에 `Ping` / `Pong` 메시지 추가
- 클라이언트가 3초마다 Ping, 서버는 Pong 응답 (또는 역방향)
- Idle Timeout 임계값을 더 짧게(5초) 낮출 수 있음
- TestClient 및 FastPortClient에 하트비트 송신 로직 추가

**거부한 안**:
- Solution B (Keep-Alive 튜닝): OS/NIC 의존성 크고, freeze 감지 불가
- Solution D/E (per-session timer): 세션 수 많을 때 타이머 객체 폭발
- Phase 2를 MVP에 포함: 프로토콜 변경은 client/server/testclient 동시 수정 필요 → 범위 증가

---

## 3. Target Users & JTBD

| Role | Job To Be Done |
|------|----------------|
| 서버 개발자 | "클라이언트가 어떻게 끊어졌든 N초 내 정리하고 싶다" |
| 운영자 | "접속자 수 지표가 실제 살아있는 세션을 반영하길 원한다" |
| 게임 클라이언트 개발자 | "서버가 나를 살아있다고 잘못 판단하지 않길 바란다" |

---

## 4. Success Criteria

| # | Criterion | Metric |
|---|-----------|--------|
| SC-1 | 비정상 단절 감지 시간 | `idleThresholdMs + 타이머 tick(1s)` 이내 (기본 설정에서 최대 11초) |
| SC-2 | IOCP/RIO 동등 동작 | 양쪽 엔진에서 동일 idle timeout 동작 확인 |
| SC-3 | 정상 세션 오탐 0 | 정상 트래픽 흐르는 세션은 절대 idle timeout으로 끊기지 않음 |
| SC-4 | 리소스 누수 없음 | TimerQueue, callback thread, RIO buffer 릭 없음 (기존 5건 수정 기조 유지) |
| SC-5 | 설정 가능 | `idleThresholdMs` 런타임/설정파일에서 조정 가능 |
| SC-6 | 성능 영향 미미 | 1만 세션 기준 타이머 tick 처리 CPU < 1% |

---

## 5. Scope

### In Scope (Phase 1)
- `TimerQueue` 래퍼 클래스 (`LibCommons` 또는 `LibNetworks`)
- `IOSession` 베이스에 `m_LastRecvTimeMs`, `UpdateLastRecvTime()` 추가
- IOCP/RIO 수신 경로에서 timestamp 업데이트
- 주기적 idle 검사 로직 (`INetworkService` 또는 `IOCPServiceMode`/`RIOServiceMode` 레벨)
- Idle 시 `RequestDisconnect()` 호출 → 기존 단절 경로 재사용
- `OnDisconnected()` 로그에 종료 사유("idle timeout") 표기
- 설정: `IdleTimeoutConfig { thresholdMs, tickIntervalMs, enabled }`

### Out of Scope (Phase 1)
- Ping/Pong 프로토콜 (Phase 2로 분리)
- 클라이언트측 수정
- 세션별 차등 타임아웃 (예: 인증 전 3초, 인증 후 30초) — 추후 확장 포인트만 준비
- 다른 타임아웃(인증 타임아웃, 핸드셰이크 타임아웃) — 별도 피처

---

## 6. Risks & Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| 타이머 콜백에서 세션 맵 순회 중 race (세션 추가/제거) | 크래시 | `shared_mutex` + 스냅샷 순회, 또는 lock-free concurrent map |
| `RequestDisconnect()` 재진입 | 이중 해제 | 기존 `m_DisconnectRequested` atomic CAS 활용 |
| TimerQueue 스레드와 IOCP/RIO 워커 간 동기화 | 데이터 경쟁 | `lastRecvTime`은 atomic, 세션 상태 변경은 기존 락/atomic 사용 |
| 대량 세션 동시 timeout → disconnect 폭주 | 부하 스파이크 | tick마다 최대 N개 세션만 처리(backpressure), 또는 stagger |
| 타이머 tick 해상도 부족 | SC-1 미달 | tickIntervalMs 기본 1000ms, 필요 시 500ms로 낮춤 |
| RIO 경로 특성상 완료 이벤트 타이밍 차이 | IOCP와 동작 불일치 | 공통 베이스 레이어에서 처리, 엔진별 분기 최소화 |

---

## 7. Key Decisions (for Plan phase)

- [ ] TimerQueue 위치: `LibCommons::Utility::TimerQueue` vs `LibNetworks` 내부
- [ ] Idle 검사 주체: `INetworkService` 공통 vs 엔진별 ServiceMode
- [ ] 세션 순회 방식: 전역 세션 맵 vs ServiceMode가 소유한 세션 리스트 스냅샷
- [ ] 기본 `idleThresholdMs` 값 (10초 제안, 게임 특성상 조정 여지)
- [ ] Phase 2(Heartbeat) 착수 시점 판단 기준

---

## 8. Next Step

`/pdca plan session-idle-timeout` 실행하여 Plan 문서 생성.
