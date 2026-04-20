# server-status Planning Document

> **Summary**: FastPort 서버에 관리 패킷 채널(Admin*)을 추가해 FastPortTestClient 가 1초 주기로 서버 상태(세션 수, bytes, CPU, 메모리 등)를 폴링·표시.
>
> **Project**: FastPort
> **Author**: AnYounggun
> **Date**: 2026-04-20
> **Status**: Draft

---

## Executive Summary

| Perspective | Content |
|-------------|---------|
| **Problem** | Windows Service 모드 서버의 내부 상태(활성 세션, bytes, idle disconnect, CPU, 메모리)를 운영·개발 중 쉽게 확인할 방법 없음. 현재는 파일 로그만 의존 |
| **Solution** | 기존 패킷 포트에 관리용 패킷 ID(`0x8001`~`0x8004`) 추가 → `AdminStatusSummary` (숫자만, 폴링용) + `AdminSessionList` (요청 시만). FastPortTestClient 에 "Admin" 탭으로 주기 폴링·표시 |
| **Function/UX Effect** | TestClient 에서 1초마다 카운터 표시, 대시보드 그래프(레이트 변화). 필요 시 세션 목록 요청 → id/lastRecvMs 테이블 출력 |
| **Core Value** | 업계 표준 "headless 서버 + 별도 관리 도구" 패턴 정착. 개발·운영·데모 시 서버 상태 가시화. 향후 Prometheus/Grafana 확장 가능한 기반 |

---

## Context Anchor

| Key | Value |
|-----|-------|
| **WHY** | Service 모드 서버 상태를 로그 외 경로로 실시간 관찰 필요 (매칭 로직 디버깅/부하 모니터링/데모) |
| **WHO** | FastPort 개발자(본인), 운영자, 데모 대상(팀 의사결정자) |
| **RISK** | Admin 패킷이 일반 클라이언트에 노출되어 DoS 소지, 세션 목록 1만개 직렬화 비용, bytes 카운터 thread-safety |
| **SUCCESS** | TestClient 에 서버 상태 실시간 표시, 1만 세션에서도 Summary 응답 < 10ms, 세션 목록은 요청 시 페이지네이션 |
| **SCOPE** | Phase 1: Summary + SessionList (페이징), FastPortServer(IOCP) + FastPortServerRIO 양쪽, FastPortTestClient Admin 탭. Out-of-scope: 인증/토큰, Prometheus exporter, 설정 변경 API |

---

## 1. Overview

### 1.1 Purpose

Windows Service 모드로 돌고 있는 FastPort 서버의 상태를 외부(테스트/관리) 클라이언트에서 패킷 채널로 실시간 조회할 수 있게 한다. 로그 파일만으로는 실시간 추이 파악이 어렵고, Service 모드에선 콘솔 출력도 제한되기 때문.

### 1.2 Background

- FastPort 는 `IOCPServiceMode` / `RIOServiceMode` 둘 다 Windows Service 로 배포되어 콘솔·GUI 없음
- 현재 관찰 수단: 파일 로그(`loggers/log_*.txt`) — 실시간성 낮음, 수치 추이 불편
- `global-timer-queue` 와 `session-idle-timeout` 가 내부 상태(이벤트 카운터, idle disconnect 횟수) 를 더 많이 쌓고 있는데 노출 채널 없음
- 업계 표준: **headless 서버 + 별도 관리 클라이언트**. FastPortTestClient (ImGui) 가 이미 있으므로 Admin 탭 추가가 자연스러움
- 향후 Prometheus/Grafana 같은 외부 모니터링으로 확장할 기초

### 1.3 Related Documents

- 선행 피처:
  - `docs/04-report/global-timer-queue.report.md`
  - `docs/04-report/session-idle-timeout.report.md`
- 프로토콜 정의: `Protocols/Benchmark.proto`, `Protocols/Tests.proto` (확장 참고)
- FastPortTestClient ImGui 패턴: `FastPortTestClient/TestClientApp.ixx`

---

## 2. Scope

### 2.1 In Scope (Phase 1)

- [ ] **프로토콜 정의**: `Protocols/Admin.proto` 신규 — AdminStatusSummary*, AdminSessionList* 메시지 4개
- [ ] **패킷 ID**: `0x8001` ~ `0x8004` (Admin 대역)
- [ ] **서버 공통 수집기**: `LibNetworks/ServerStatsCollector.ixx` — Uptime/CPU/Memory/ServerMode/IdleDisconnect 카운터 집계
- [ ] **IOSession 바이트 카운터**: `atomic<uint64_t> m_TotalRxBytes / m_TotalTxBytes` 추가, OnIOCompleted 수신·송신 경로에서 갱신
- [ ] **세션 합산**: `ServerStatsCollector` 가 `SessionContainer::ForEach` 로 순회 합산
- [ ] **Admin 패킷 핸들러**: `IOCPInboundSession` / `RIOInboundSession` 의 `OnPacketReceived` 에 `0x8001`/`0x8003` 분기 추가
- [ ] **페이지네이션**: `AdminSessionListRequest { offset, limit }` — 기본 100, 최대 1000
- [ ] **FastPortTestClient Admin 탭**:
  - Summary 주기 폴링 (1초) — 숫자 + 간단 그래프(rx/tx bytes rate, active sessions)
  - SessionList 버튼 요청 — 페이징 테이블
- [ ] **서버 공통화**: IOCP/RIO 양쪽에 동일 `AdminPacketHandler` 적용 (LibNetworks 에 free function 또는 static 헬퍼)
- [ ] **단위 테스트**:
  - `ServerStatsCollector` CPU/Memory/Uptime 값 산출 검증
  - Admin 패킷 파싱/생성 왕복 테스트
  - Session counter (bytes) 갱신 검증
- [ ] **Logger 연동**: Admin 요청 수신/응답 로그 (`"AdminHandler"` 카테고리)

### 2.2 Out of Scope

- **인증/토큰**: 내부 도구 전제. 추후 추가 여지만 (proto 에 `auth_token` optional 필드 예약)
- **설정 변경 API** (ReloadConfig 등): 상태 조회 전용. 변경 기능은 별도 피처
- **Prometheus HTTP 엔드포인트**: 후속 피처
- **세션 상세 정보** (IP, 연결 시간 등): 이번엔 id/lastRecvMs 만
- **Disconnect Session API**: 관리 기능은 후속 피처
- **FastPortClient** 이중 용도: TestClient 만 Admin 기능 추가. 일반 클라이언트엔 없음
- **Push/Streaming**: 폴링만. Streaming 은 후속

---

## 3. Requirements

### 3.1 Functional Requirements

| ID | Requirement | Priority | Status |
|----|-------------|----------|--------|
| FR-01 | `Protocols/Admin.proto` 정의 (4 메시지) | High | Pending |
| FR-02 | `IOSession::m_TotalRxBytes / m_TotalTxBytes` atomic 추가 + OnIOCompleted 경로 갱신 | High | Pending |
| FR-03 | `ServerStatsCollector` 클래스 (Uptime/CPU/Memory/IdleDisconnect/ServerMode 수집) | High | Pending |
| FR-04 | `AdminStatusSummaryRequest → Response` 핸들러 (IOCP) | High | Pending |
| FR-05 | `AdminSessionListRequest → Response` 핸들러, offset/limit 페이징 | High | Pending |
| FR-06 | RIOInboundSession 에도 동일 핸들러 | High | Pending |
| FR-07 | 공통 Admin 핸들러 헬퍼 (LibNetworks 또는 각 세션에서 호출) | Medium | Pending |
| FR-08 | FastPortTestClient "Admin" 탭 신설 (Summary 주기 폴링 + SessionList 요청 버튼) | High | Pending |
| FR-09 | Admin 요청/응답 로그 (`"AdminHandler"` 카테고리) | Medium | Pending |
| FR-10 | 알 수 없는 패킷 ID(0x8xxx) 은 경고 로그만, 연결 유지 | Medium | Pending |

### 3.2 Non-Functional Requirements

| Category | Criteria | Measurement Method |
|----------|----------|-------------------|
| **Summary latency** | 1만 세션 시 Request → Response ≤ 10ms | TestClient 측에서 송수신 타임스탬프 |
| **SessionList latency** | 100 세션 페이지 ≤ 5ms, 1000 세션 페이지 ≤ 20ms | 동일 |
| **Polling 부하** | TestClient 1개 + 1Hz polling 시 서버 CPU 영향 미미 (< 0.5%) | 장시간 실행 관찰 |
| **Thread-safety** | bytes 카운터 atomic relaxed, Collector 단일 호출 시점 일관성 | 코드 리뷰 + 테스트 |
| **Memory 추가 비용** | 세션당 16 bytes (rx+tx atomic) 추가, 1만 세션 = 160KB | 설계 상 산출 |
| **Build** | MSVC 2022 x64 Debug/Release 모두 빌드 성공 | FastPort.slnx |

---

## 4. Success Criteria

### 4.1 Definition of Done

- [ ] FR-01 ~ FR-10 모두 구현
- [ ] 단위 테스트 신규 (≥ 3개):
  - Admin 프로토콜 파싱/생성 왕복
  - IOSession bytes 카운터 갱신
  - ServerStatsCollector 값 산출 (CPU/Memory/Uptime)
- [ ] 수동 시나리오:
  - FastPortServer 실행 → TestClient Admin 탭에서 Summary 값 1초마다 갱신 확인
  - 활성 세션 여러 개 생성 후 SessionList 페이징 확인
  - FastPortServerRIO 도 동일 동작
- [ ] 빌드 경고 0
- [ ] 기존 테스트 회귀 없음

### 4.2 Quality Criteria

- [ ] 단위 테스트 전부 pass
- [ ] MSVC Level3 warning 0
- [ ] Admin 로직이 일반 패킷 처리 속도에 영향 미미 (별도 경로)

---

## 5. Risks and Mitigation

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| **Admin 패킷이 일반 클라에 노출 → DoS/정보 누출 소지** | Medium | Medium | Phase 1 은 내부 망/개발 환경 전용 전제. Future Work 에 `auth_token` 추가 명시 |
| **SessionList 1만 세션 직렬화 부하** | High | Low | 페이지네이션 (기본 100, 최대 1000) 강제. 폴링은 Summary 만, SessionList 는 요청 시에만 |
| **bytes 카운터 성능 저하** | Medium | Low | `memory_order_relaxed` atomic. Hot path 의 fetch_add 1회만. 기존 `m_LastRecvTimeMs` 업데이트 근처에 두어 캐시 친화 |
| **CPU 샘플링이 2회 측정 간 차이로 비직관** | Low | Medium | `ServerStatsCollector` 가 내부에 이전 샘플 보관. 첫 호출 시 0%, 이후 delta 계산 |
| **RIO 세션 bytes 카운터 — RIOSession 과 IOSession 계층 차이** | Medium | Medium | `IIdleAware` 와 유사하게 공통 인터페이스 추가 or `IOSession` 베이스에 멤버 두고 RIOSession 도 갱신 (RIOSession 이 IOSession 을 상속하지 않음을 고려) |
| **FastPortTestClient Admin 탭 ImGui 복잡도** | Medium | Low | 기존 탭 구조 재사용. 테이블(ImGui::BeginTable) + 간단 라인 그래프(ImPlot)만 |
| **패킷 ID 충돌** | Low | Low | `0x8xxx` 대역은 Benchmark/Echo 와 분리. `Protocols/Admin.proto` 에 범위 주석 |
| **Admin 로직이 일반 비즈니스 로직과 섞여 유지보수 악화** | Medium | Medium | `AdminPacketHandler` 로 분리해 `OnPacketReceived` 는 dispatch 만 |

---

## 6. Impact Analysis

### 6.1 Changed Resources

| Resource | Type | Change Description |
|----------|------|--------------------|
| `Protocols/Admin.proto` | New | 4개 메시지 + enum |
| `Protocols/Protocols.vcxproj(.filters)` | Modified | Admin.proto 등록 |
| `LibNetworks/IOSession.ixx` | Modified | `atomic<uint64_t> m_TotalRxBytes, m_TotalTxBytes` + 접근자 |
| `LibNetworks/IOSession.cpp` | Modified | OnIOCompleted 수신 경로에서 `m_TotalRxBytes += bytesTransferred`, 송신 경로에서 `m_TotalTxBytes += bytesTransferred` |
| `LibNetworks/RIOSession.{ixx,cpp}` | Modified | 동일 바이트 카운터 추가 (IOSession 과 독립) |
| `LibNetworks/ServerStatsCollector.{ixx,cpp}` | New | CPU/Memory/Uptime/ServerMode/IdleDisconnectCount 수집 |
| `LibNetworks/AdminPacketHandler.{ixx,cpp}` | New | Admin 패킷 처리 공통 헬퍼 (Summary + SessionList) |
| `FastPortServer/IOCPInboundSession.cpp` | Modified | `OnPacketReceived` 에 Admin 패킷 분기 추가 |
| `FastPortServerRIO/RIOInboundSession.cpp` | Modified | 동일 |
| `FastPortServer/IOCPServiceMode.cpp` | Modified | ServerStatsCollector 인스턴스 보유 + SessionContainer 를 Collector 에 주입 |
| `FastPortServerRIO/RIOServiceMode.cpp` | Modified | 동일 |
| `FastPortTestClient/` (신규 또는 수정) | Modified/New | Admin 탭 UI + 폴링 로직 |
| `LibNetworksTests/AdminProtocolTests.cpp` (또는 ServerStatsCollectorTests.cpp) | New | 단위 테스트 |

### 6.2 Current Consumers

| Resource | Operation | Code Path | Impact |
|----------|-----------|-----------|--------|
| `IOSession::OnIOCompleted` | 수신/송신 경로 확장 | IOSession.cpp | 경량 `atomic::fetch_add` 1회 추가 — 성능 영향 무시 가능 |
| `IOCPInboundSession::OnPacketReceived` | 패킷 dispatch 확장 | 기존 switch 에 case 2개 추가 | 기존 ECHO/BENCHMARK 영향 없음 |
| `RIOInboundSession::OnPacketReceived` | 동일 | 동일 | 동일 |
| `SessionContainer` (SingleTon) | ForEach 추가 소비자 | ServerStatsCollector 가 read-only | 기존 Add/Remove 영향 없음 (idle-timeout 과 함께 공유) |

### 6.3 Verification

- [ ] 기존 Echo/Benchmark 패킷이 여전히 정상 동작
- [ ] IOCP / RIO 서버 모두 실행 후 TestClient 연결 정상
- [ ] SessionIdleChecker 와 동시 운영 시 상태 조회 결과 일관 (IdleDisconnectCount 일치)

---

## 7. Architecture Considerations

### 7.1 Project Level

| Level | Selected |
|-------|:--------:|
| Starter | ☐ |
| Dynamic | ☐ |
| **Enterprise** (C++ native, 고성능) | ☑ |

### 7.2 Key Architectural Decisions

| Decision | Options | Selected | Rationale |
|----------|---------|----------|-----------|
| 채널 | 별도 포트 / 기존 포트+새 ID / gRPC 별도 서비스 | **기존 포트+새 ID** | Q2 확정 — 변경 최소, protobuf 인프라 재사용 |
| 메시지 분리 | 단일 Status / Summary+SessionList 분리 | **분리** | Q6 추천 — 폴링은 가벼운 Summary 만, 목록은 요청 시 |
| bytes 수집 | 전역 / 세션별 | **세션별 atomic + Collector 합산** | Q7 추천 — 향후 세션 상세 확장 여지 |
| Stats 수집기 | 서버 모드별 / 공용 | **공용 `ServerStatsCollector`** | Q8 동의 — IOCP/RIO 동일 인터페이스 |
| 적용 범위 | IOCP 만 / 둘 다 | **IOCP + RIO** | Q9 확정 |
| 패킷 ID 대역 | 순차 / admin 대역 | **0x8001~ admin 대역** | Q10 확정 — 향후 admin 기능 확장 공간 |
| 인증 | 무 / 토큰 / IP 제한 | **무 (Phase 1)** | Q4 확정. `auth_token` 옵셔널 필드 proto 에 예약 |
| 클라이언트 UI | ImGui Admin 탭 / 별도 GUI | **FastPortTestClient Admin 탭** | 기존 도구 재사용 |
| CPU 측정 | `GetProcessTimes` / perfmon | **GetProcessTimes (샘플링)** | 의존성 최소, Windows 네이티브 |
| Logger | LibCommons::Logger + GMF spdlog 헤더 | 동일 | CLAUDE.md 지침 준수 |

### 7.3 모듈 관계 (개략)

```
Protocols/Admin.proto
        │
        ▼
[Protocols.vcxproj] — AdminStatusSummary{Request,Response}, AdminSessionList{Request,Response}

LibNetworks/
├── IOSession.{ixx,cpp}           (수정: m_TotalRxBytes, m_TotalTxBytes)
├── RIOSession.{ixx,cpp}           (수정: 동일 바이트 카운터)
├── ServerStatsCollector.{ixx,cpp} (신규: Uptime/CPU/Memory/IdleCount/ServerMode)
└── AdminPacketHandler.{ixx,cpp}   (신규: Summary/SessionList 빌드 + 응답 송신 헬퍼)

FastPortServer / FastPortServerRIO:
├── *InboundSession.cpp            (수정: OnPacketReceived Admin 분기)
└── *ServiceMode.cpp               (수정: ServerStatsCollector 인스턴스 보유)

FastPortTestClient:
└── (신규 또는 수정) Admin 탭 UI  (1Hz 폴링 + 페이징 테이블)

LibNetworksTests:
└── ServerStatsTests.cpp / AdminProtocolTests.cpp (신규)
```

### 7.4 API 스케치

```proto
// Protocols/Admin.proto (대략)
syntax = "proto3";
package fastport.protocols.admin;

enum ServerMode {
  SERVER_MODE_UNKNOWN = 0;
  SERVER_MODE_IOCP    = 1;
  SERVER_MODE_RIO     = 2;
}

message AdminStatusSummaryRequest {
  uint64 request_id = 1;
  string auth_token = 2;  // Phase 1: 무시. Future Work 예약.
}

message AdminStatusSummaryResponse {
  uint64     request_id            = 1;
  uint64     server_uptime_ms      = 2;
  uint32     active_session_count  = 3;
  uint64     total_rx_bytes        = 4;
  uint64     total_tx_bytes        = 5;
  uint64     idle_disconnect_count = 6;
  ServerMode server_mode           = 7;
  uint64     process_memory_bytes  = 8;
  double     process_cpu_percent   = 9;
  uint64     server_timestamp_ms   = 10;
}

message AdminSessionListRequest {
  uint64 request_id = 1;
  uint32 offset     = 2;
  uint32 limit      = 3;  // 서버가 1000으로 clamp
  string auth_token = 4;
}

message AdminSessionInfo {
  uint64 session_id      = 1;
  int64  last_recv_ms    = 2;
  uint64 rx_bytes        = 3;
  uint64 tx_bytes        = 4;
}

message AdminSessionListResponse {
  uint64                     request_id = 1;
  uint32                     total      = 2;
  uint32                     offset     = 3;
  repeated AdminSessionInfo  sessions   = 4;
}
```

---

## 8. Convention Prerequisites

### 8.1 Existing Project Conventions

- [x] `CLAUDE.md` — 로깅 정책, 타이머 수명, C++ 표준 지침
- [x] `Protocols/` 디렉토리, .proto → protoc → pb.cc/h 자동 빌드
- [x] `IOSession` atomic 멤버 패턴 (기존 `m_LastRecvTimeMs`)
- [x] Logger 카테고리 — 신규 `"AdminHandler"`, `"ServerStats"` 추가

### 8.2 To Define

| Category | Current | To Define |
|---|---|---|
| Admin 패킷 ID 대역 | 없음 | `0x8000~0x8FFF` admin 용 예약, CLAUDE.md 에 기록 |
| `auth_token` 필드 | 없음 | proto 에 필드 예약, 서버는 Phase 1 에서 무시 |

### 8.3 Environment Variables

불필요.

---

## 9. Next Steps

1. [ ] Design 문서 (`/pdca design server-status`)
   - 3가지 아키텍처 옵션 (A Minimal / B Clean / C Pragmatic)
   - IOCP/RIO 공통화 세부 (IOSession vs RIOSession 각자 구현 vs 공용 베이스)
   - CPU/Memory 샘플링 정확도 (GetProcessTimes delta 계산)
2. [ ] Do 구현 (스코프 분할 예상: `proto,stats` / `handler` / `testclient-ui` / `integration`)
3. [ ] Check / Report

---

## 10. Future Work (Phase 2+)

- **인증 (auth_token 실활성화)**: 관리자 토큰 확인 + 감사 로그
- **Disconnect Session API**: 특정 세션 id 강제 종료
- **ReloadConfig API**: idle threshold 등 런타임 변경
- **Prometheus HTTP `/metrics`**: 별도 포트(8080) 에 노출 → Grafana
- **세션 상세 정보**: IP, 연결 시각, 프로토콜 버전
- **Streaming**: 서버 push 방식 (요청 후 N초마다 자동 전송)
- **서버 측 샘플링**: CPU/Memory 를 외부 polling 없이 주기 로깅

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-04-20 | Initial draft (Q1~Q11 전부 권장안 확정) | AnYounggun |
