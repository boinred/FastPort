# test-client-ui Planning Document

> **Summary**: FastPort IOCP/RIO 듀얼 엔진을 시각적으로 검증/데모할 수 있는 ImGui 기반 GUI 테스트 클라이언트
>
> **Project**: FastPort
> **Author**: AnYounggun
> **Date**: 2026-04-15
> **Status**: Draft

---

## Executive Summary

| Perspective | Content |
|-------------|---------|
| **Problem** | 콘솔 기반 클라이언트/벤치마크만 존재하여 IOCP/RIO 듀얼 엔진의 동작과 성능 차이를 시각적으로 검증하거나 비개발자에게 데모할 방법이 없음 |
| **Solution** | ImGui + implot 기반 GUI 테스트 클라이언트. 실시간 메트릭 차트, 에코/스케일 테스트, A/B 비교 모드 제공 |
| **Function/UX Effect** | 버튼 클릭으로 연결/테스트 수행, 실시간 차트로 레이턴시/처리량 시각화, IOCP vs RIO 나란히 비교 |
| **Core Value** | 엔진 동작 가시성 확보, behavioral parity 시각적 검증, 사내 채택 설득력 강화 |

---

## Context Anchor

| Key | Value |
|-----|-------|
| **WHY** | 콘솔 로그/CSV만으로는 엔진 동작 검증과 팀 설득이 어려움 |
| **WHO** | FastPort 개발팀 (검증), 사내 기술 의사결정자 (데모) |
| **RISK** | ImGui + C++20 모듈 통합 호환성, 멀티 연결 시 GUI 스레드 블로킹 |
| **SUCCESS** | IOCP/RIO 양 모드 연결 + 실시간 차트 + A/B 비교 동작 |
| **SCOPE** | Phase 1: 기본 연결/에코/메트릭, Phase 2: A/B 비교 + 스케일 테스트 |

---

## 1. Overview

### 1.1 Purpose

FastPort 엔진의 IOCP/RIO 듀얼 모드를 GUI 환경에서 인터랙티브하게 테스트하고, 성능 차이를 실시간 차트로 시각화하는 개발/데모 도구.

### 1.2 Background

- FastPort는 현재 `FastPortServer`, `FastPortClient`, `FastPortBenchmark` 3개의 콘솔 앱만 보유
- `FastPortClient`는 단일 연결 에코 테스트만 가능
- `FastPortBenchmark`는 레이턴시 측정을 하지만 결과가 콘솔 텍스트 출력
- 디자인 문서(office-hours)에서 "팀 의사결정자에게 보여줄 산출물" 필요성이 확인됨
- A/B 비교(IOCP vs RIO)를 시각적으로 보여주는 것이 채택 설득의 핵심

### 1.3 Related Documents

- PRD: `docs/00-pm/test-client-ui.prd.md`
- Design Doc (office-hours): `~/.gstack/projects/boinred-FastPort/AzureAD+AnYounggun-main-design-20260413-154324.md`

---

## 2. Scope

### 2.1 In Scope

- [ ] ImGui + implot + DX11 백엔드 통합 (vcpkg)
- [ ] 서버 연결/해제 UI (IP, Port, 모드 선택)
- [ ] 에코 테스트 (단일/반복, 왕복 시간 측정)
- [ ] 실시간 메트릭 표시 (활성 세션, msg/sec, p50/p95/p99 레이턴시)
- [ ] 레이턴시/처리량 롤링 차트 (implot)
- [ ] 동시 접속 스케일 테스트 (N개 연결 생성)
- [ ] A/B 비교 모드 (IOCP 서버 + RIO 서버 동시 연결, 결과 나란히)
- [ ] 세션 로그 뷰 (패킷 송수신 기록)

### 2.2 Out of Scope

- 서버 측 메트릭 API (서버에서 메트릭을 푸시하는 건 별도 작업. 클라이언트 측 측정만 사용)
- 크로스 플랫폼 지원 (Windows 전용)
- 프로덕션 모니터링 도구 (개발/테스트 전용)
- protobuf 메시지 에디터 (에코 테스트에 고정 메시지 사용)

---

## 3. Requirements

### 3.1 Functional Requirements

| ID | Requirement | Priority | Status |
|----|-------------|----------|--------|
| FR-01 | ImGui 창에서 서버 IP/Port/모드(IOCP or RIO) 입력 후 연결/해제 | P0 | Pending |
| FR-02 | 에코 메시지 전송 + 왕복 시간 측정 (단발/연속) | P0 | Pending |
| FR-03 | 실시간 메트릭 패널: 활성 세션 수, msg/sec, 평균/p95/p99 레이턴시 | P0 | Pending |
| FR-04 | implot 기반 롤링 차트 (60초 윈도우, 레이턴시 + 처리량) | P1 | Pending |
| FR-05 | N개 동시 연결 생성 (1, 10, 100, 1000) 스케일 테스트 | P1 | Pending |
| FR-06 | A/B 비교 모드: 2개 서버 동시 연결, 동일 워크로드, 결과 나란히 표시 | P1 | Pending |
| FR-07 | 대형 패킷 전송 테스트 (버퍼 사이즈 초과) | P2 | Pending |
| FR-08 | 세션 로그 뷰 (송수신 패킷 타임스탬프 + 크기 스크롤) | P2 | Pending |
| FR-09 | 테스트 결과 CSV 내보내기 | P3 | Pending |

### 3.2 Non-Functional Requirements

| Category | Criteria | Measurement Method |
|----------|----------|-------------------|
| Performance | GUI 60fps 유지 (네트워크 I/O가 렌더링 블로킹하지 않음) | ImGui 프레임 타이밍 |
| Usability | 비개발자가 3분 내 데모 완료 가능 | 수동 테스트 |
| Build | vcpkg manifest 모드로 의존성 관리, 단일 .exe 산출 | 빌드 스크립트 |
| Compatibility | 기존 FastPortServer(IOCP/RIO 양 모드)와 완전 호환 | 연결 테스트 |

---

## 4. Success Criteria

### 4.1 Definition of Done

- [ ] ImGui 창에서 FastPort 서버(IOCP 모드)에 연결 성공
- [ ] ImGui 창에서 FastPort 서버(RIO 모드)에 연결 성공
- [ ] 에코 테스트 왕복 시간 실시간 표시
- [ ] p50/p95/p99 레이턴시 implot 차트 렌더링
- [ ] 100개 동시 연결 스케일 테스트 수행 가능
- [ ] A/B 비교 모드에서 IOCP vs RIO 나란히 차트 렌더링
- [ ] 빌드: 오류 0개, 경고 0개

### 4.2 Quality Criteria

- [ ] GUI 스레드와 네트워크 I/O 스레드 분리 (블로킹 없음)
- [ ] 1000개 연결에서 메모리 누수 없음 (30분 테스트)
- [ ] 기존 FastPortServer 코드 수정 없이 동작

---

## 5. Risks and Mitigation

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| ImGui + C++20 모듈 빌드 호환성 | High | Medium | PoC 먼저 수행 (빈 창 띄우기). ImGui는 헤더 온리이므로 모듈과 분리 가능 |
| 네트워크 I/O가 GUI 스레드 블로킹 | High | High | 별도 I/O 스레드에서 IOService/RIOService 구동, GUI는 메인 스레드에서 폴링만 |
| 1000개 동시 연결 시 클라이언트 포트 고갈 | Medium | Low | ephemeral port 범위 확대 + 연결 재사용 |
| implot 실시간 차트 성능 (대량 데이터 포인트) | Medium | Low | 롤링 윈도우 60초로 데이터 포인트 제한 |

---

## 6. Impact Analysis

### 6.1 Changed Resources

| Resource | Type | Change Description |
|----------|------|--------------------|
| vcpkg.json | Config | `imgui`, `implot` 의존성 추가 |
| FastPort.slnx | Config | `FastPortTestClient` 프로젝트 추가 |

### 6.2 Current Consumers

| Resource | Operation | Code Path | Impact |
|----------|-----------|-----------|--------|
| LibNetworks.lib | LINK | FastPortClient, FastPortServer, FastPortBenchmark | None (새 프로젝트 추가만) |
| Protocols.lib | LINK | FastPortClient, FastPortServer | None |
| vcpkg.json | BUILD | 전체 솔루션 | imgui/implot 추가로 빌드 시간 소폭 증가 |

### 6.3 Verification

- [x] 기존 프로젝트 빌드에 영향 없음 (새 프로젝트 추가만)
- [x] 기존 라이브러리 코드 수정 없음
- [x] 서버 측 수정 없음

---

## 7. Architecture Considerations

### 7.1 Project Level

C++ 네이티브 프로젝트 (bkit 레벨 체계 비해당). Visual Studio 2022+ / C++20 모듈.

### 7.2 Key Architectural Decisions

| Decision | Options | Selected | Rationale |
|----------|---------|----------|-----------|
| GUI 프레임워크 | ImGui / WPF / Web | **ImGui** | C++ 직접 링크, 게임 업계 표준, 의존성 최소 |
| 차트 라이브러리 | implot / custom | **implot** | ImGui와 완전 통합, 실시간 차트 네이티브 지원 |
| 렌더링 백엔드 | DX11 / DX12 / OpenGL | **DX11** | Windows 전용이므로 최적, ImGui 공식 지원 |
| 스레딩 모델 | 단일 스레드 / GUI+IO 분리 | **GUI+IO 분리** | GUI 60fps 유지 필수, IO가 블로킹하면 안됨 |
| 네트워크 엔진 | LibNetworks 직접 사용 / 별도 래퍼 | **직접 사용** | 기존 IOService + IOSocketConnector 재사용 |

### 7.3 Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                  FastPortTestClient.exe                   │
├──────────────────────┬──────────────────────────────────┤
│   Main Thread (GUI)  │      IO Thread(s)                │
│                      │                                  │
│  ImGui Render Loop   │  IOService (IOCP worker pool)    │
│  ┌──────────────┐   │  ┌─────────────────────────────┐ │
│  │ Dashboard     │   │  │ TestSession(s)              │ │
│  │ - Metrics     │◄──┼──│ - Echo RTT 측정             │ │
│  │ - Charts      │   │  │ - 패킷 카운터              │ │
│  │ - Log View    │   │  │ - 타임스탬프 기록          │ │
│  ├──────────────┤   │  └─────────────────────────────┘ │
│  │ Controls      │───┤                                  │
│  │ - Connect     │   │  IOSocketConnector               │
│  │ - Test Run    │   │  ┌─────────────────────────────┐ │
│  │ - A/B Mode    │   │  │ N connections → Server      │ │
│  └──────────────┘   │  └─────────────────────────────┘ │
│                      │                                  │
│  MetricsCollector    │  (lock-free queue로 메트릭 전달)  │
│  (atomic counters)   │                                  │
└──────────────────────┴──────────────────────────────────┘

데이터 흐름:
IO Thread → atomic counters → GUI Thread (매 프레임 읽기)
GUI Thread → command queue → IO Thread (연결/테스트 명령)
```

---

## 8. File Structure

```
FastPortTestClient/
├── FastPortTestClient.vcxproj
├── FastPortTestClient.cpp          # main() - ImGui 초기화 + 메인 루프
├── TestClientApp.ixx               # 앱 상태 관리 + ImGui 렌더링
├── TestSession.ixx                 # IOSession 확장 - RTT 측정 + 패킷 카운터
├── MetricsCollector.ixx            # atomic 카운터 + 통계 계산 (p50/p95/p99)
├── TestRunner.ixx                  # 에코/플러드/스케일 테스트 로직
└── ABCompare.ixx                   # A/B 비교 모드 (2개 서버 동시 연결)
```

---

## 9. Implementation Phases

### Phase 1: 기본 기능 (P0) — 예상 2-3일

1. vcpkg에 imgui, implot 추가
2. FastPortTestClient.vcxproj 생성 (DX11 백엔드)
3. ImGui 빈 창 + 기본 레이아웃
4. TestSession 구현 (IOSession 확장, RTT 측정)
5. 연결/해제 UI + 에코 테스트
6. 실시간 메트릭 패널

### Phase 2: 시각화 + 고급 기능 (P1) — 예상 2-3일

7. implot 레이턴시/처리량 롤링 차트
8. 동시 접속 스케일 테스트
9. A/B 비교 모드

### Phase 3: 부가 기능 (P2-P3) — 예상 1일

10. 대형 패킷 테스트
11. 세션 로그 뷰
12. CSV 내보내기

---

## 10. Next Steps

1. [ ] `/pdca design test-client-ui` — 상세 설계 문서 작성
2. [ ] ImGui PoC: vcpkg 통합 + 빈 창 렌더링 확인
3. [ ] 팀 리뷰 및 승인
4. [ ] 구현 시작

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-04-15 | Initial draft (PRD 기반) | AnYounggun |
