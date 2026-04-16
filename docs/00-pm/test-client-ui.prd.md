# test-client-ui - Product Requirements Document

> **Date**: 2026-04-15
> **Author**: AnYounggun
> **Method**: bkit PM Analysis (manual, Agent Teams unavailable)
> **Status**: Draft

---

## Executive Summary

| Perspective | Content |
|-------------|---------|
| **Problem** | FastPort 엔진은 콘솔 기반 서버/클라이언트/벤치마크만 존재하여, IOCP/RIO 듀얼 엔진의 동작을 시각적으로 검증하거나 데모할 방법이 없다 |
| **Solution** | 실시간 메트릭 대시보드 + 인터랙티브 테스트 기능을 갖춘 GUI 테스트 클라이언트 |
| **Target User** | FastPort 개발팀 (본인 포함), 사내 기술 의사결정자 (데모 대상) |
| **Core Value** | 엔진 동작 가시성 확보, behavioral parity 시각적 검증, 사내 채택 설득력 강화 |

---

## 1. Opportunity Discovery

### 1.1 Desired Outcome

FastPort의 IOCP/RIO 듀얼 엔진 성능 차이와 동작 호환성을 비개발자도 이해할 수 있는 형태로 시각화하여, 사내 채택 결정을 가속화한다.

### 1.2 Brainstormed Ideas (Top 5)

| # | Idea | Perspective | Rationale |
|---|------|-------------|-----------|
| 1 | **Windows GUI 테스트 클라이언트** (WinUI 3 / WPF) | Native | C++ 엔진과 같은 생태계, 직접 링크 가능. Windows 전용이라 제약 없음 |
| 2 | **웹 대시보드** (React + WebSocket 브릿지) | Cross-platform | 브라우저만 있으면 접근 가능. 하지만 C++ 서버에 WebSocket 레이어 추가 필요 |
| 3 | **ImGui 기반 오버레이** | Lightweight | 게임 업계 표준 디버그 UI. C++에서 직접 사용. 빌드 의존성 최소 |
| 4 | **터미널 TUI** (ncurses/FTXUI) | Minimal | 추가 의존성 적고 SSH 원격에서도 사용 가능 |
| 5 | **Electron 앱** | Rich UI | 풍부한 차트/그래프. 하지만 C++ 바인딩 복잡 |

### 1.3 Opportunity Solution Tree

```
Outcome: 엔진 동작을 시각적으로 검증하고 데모할 수 있다
├── Opportunity 1: 실시간 성능 가시화
│   ├── Solution A: ImGui 대시보드 (엔진 내장)
│   └── Solution B: 웹 대시보드 (별도 프로세스)
├── Opportunity 2: 인터랙티브 테스트 수행
│   ├── Solution C: GUI에서 연결/메시지 조작
│   └── Solution D: 스크립트 기반 시나리오 실행
└── Opportunity 3: 사내 데모/설득 도구
    ├── Solution E: A/B 비교 화면 (IOCP vs RIO 나란히)
    └── Solution F: 벤치마크 결과 리포트 자동 생성
```

### 1.4 Prioritized Opportunities

| # | Opportunity | Importance | Satisfaction | Score |
|---|------------|:----------:|:------------:|:-----:|
| 1 | 실시간 성능 가시화 | 0.9 | 0.1 | 0.81 |
| 2 | 인터랙티브 테스트 수행 | 0.8 | 0.2 | 0.64 |
| 3 | 사내 데모/설득 도구 | 0.7 | 0.0 | 0.49 |

### 1.5 Key Assumptions & Risk Prioritization

| # | Assumption | Category | Impact | Risk | Score | Action |
|---|-----------|----------|:------:|:----:|:-----:|--------|
| 1 | ImGui가 C++20 모듈 프로젝트에 통합 가능 | Technical | H | M | 6 | PoC 먼저 |
| 2 | 엔진 메트릭을 실시간으로 뽑을 수 있는 인터페이스 존재 | Technical | H | L | 4 | 현재 spdlog 로그만 존재, 메트릭 API 추가 필요 |
| 3 | 비개발자가 데모를 보고 기술적 차이를 이해할 수 있음 | UX | M | M | 4 | 시각화 디자인 중요 |
| 4 | 팀에서 GUI 클라이언트를 유지보수할 의향이 있음 | Org | M | H | 6 | 최소 의존성으로 유지보수 부담 최소화 |

### 1.6 Recommended Experiments

| # | Tests Assumption | Method | Success Criteria | Effort |
|---|-----------------|--------|-----------------|:------:|
| 1 | ImGui + C++20 모듈 통합 | vcpkg로 ImGui 추가 후 빈 창 띄우기 | 빌드 성공 + 창 렌더링 | S |
| 2 | 엔진 메트릭 수집 | RIOService/IOService에 카운터 추가 | 초당 처리량, 활성 세션 수 실시간 조회 | S |
| 3 | A/B 비교 화면 | IOCP/RIO 서버 2개 동시 실행 + 병렬 연결 | 같은 워크로드에서 차이 시각화 | M |

---

## 2. Value Proposition & Strategy

### 2.1 JTBD Value Proposition (6-Part)

| Part | Content |
|------|---------|
| **Who** | 사내 게임 서버 개발팀 + 기술 의사결정자 |
| **When** | FastPort 엔진을 평가/검증/데모할 때 |
| **I want to** | IOCP와 RIO의 동작/성능 차이를 실시간으로 눈으로 확인하고 |
| **So I can** | 엔진 선택에 대한 데이터 기반 의사결정을 하고 팀에 설득력 있게 제안할 수 있다 |
| **Unlike** | 콘솔 로그와 CSV 벤치마크 결과만 보는 현재 방식 |
| **Our solution** | 실시간 차트, 인터랙티브 테스트, A/B 비교 기능을 갖춘 GUI 테스트 클라이언트 |

---

## 3. Recommended Approach: ImGui 기반 테스트 클라이언트

### 3.1 Why ImGui

| 기준 | ImGui | WPF/WinUI | Web (React) | Electron |
|------|-------|-----------|-------------|----------|
| C++ 통합 | 직접 링크 | C++/CLI 필요 | WebSocket 브릿지 | N-API 바인딩 |
| 빌드 의존성 | vcpkg 1줄 | .NET SDK | Node.js | Node.js + Chromium |
| 게임 업계 친숙도 | 매우 높음 | 보통 | 낮음 | 낮음 |
| 유지보수 부담 | 최소 | 중간 | 높음 | 높음 |
| 시각화 품질 | 좋음 (implot) | 매우 좋음 | 매우 좋음 | 매우 좋음 |
| **추천** | **1순위** | 2순위 | 3순위 | 비추천 |

ImGui는 게임 서버 팀이 이미 친숙한 도구이고, C++ 프로젝트에 `vcpkg add imgui` 한 줄로 통합됩니다. implot 라이브러리로 실시간 차트도 가능.

### 3.2 핵심 화면 구성

```
┌─────────────────────────────────────────────────────────┐
│  FastPort Test Client                          [IOCP|RIO]│
├──────────────┬──────────────────────────────────────────┤
│              │  ┌─ Real-time Metrics ──────────────────┐│
│  Controls    │  │ Active Sessions: 1,234              ││
│              │  │ Messages/sec:    45,678              ││
│  Server:     │  │ Avg Latency:     0.23ms             ││
│  127.0.0.1   │  │ p99 Latency:     1.2ms              ││
│  Port: 6628  │  │ CPU Usage:       34%                ││
│              │  └─────────────────────────────────────┘│
│  [Connect]   │                                         │
│  [Disconnect]│  ┌─ Latency Chart (rolling 60s) ───────┐│
│              │  │  ╱╲    ╱╲                            ││
│  Connections:│  │ ╱  ╲╱╱  ╲   IOCP (blue)             ││
│  [1] [10]    │  │╱        ╲  RIO  (green)             ││
│  [100][1000] │  └─────────────────────────────────────┘│
│              │                                         │
│  Test:       │  ┌─ Throughput Chart ──────────────────┐│
│  [Echo Test] │  │ ████████████████████ IOCP: 32K/s    ││
│  [Flood Test]│  │ ██████████████████████████ RIO: 48K/s││
│  [Large Pkt] │  └─────────────────────────────────────┘│
│              │                                         │
│  A/B Compare:│  ┌─ Session Log ───────────────────────┐│
│  [Start A/B] │  │ 08:23:01 [RIO] Session #42 accepted ││
│              │  │ 08:23:01 [RIO] Echo: 128B, 0.1ms    ││
│              │  │ 08:23:02 [IOCP] Session #43 accepted││
│              │  └─────────────────────────────────────┘│
└──────────────┴──────────────────────────────────────────┘
```

### 3.3 기능 목록

| # | 기능 | 우선순위 | 설명 |
|---|------|---------|------|
| F1 | 서버 연결/해제 | P0 | IP/Port 입력, IOCP/RIO 모드 선택, 연결 상태 표시 |
| F2 | 실시간 메트릭 표시 | P0 | 활성 세션 수, msg/sec, 레이턴시 (p50/p95/p99) |
| F3 | 에코 테스트 | P0 | 메시지 보내고 왕복 시간 측정 |
| F4 | 레이턴시 차트 | P1 | implot 기반 롤링 60초 실시간 그래프 |
| F5 | 동시 접속 스케일 테스트 | P1 | N개 연결 생성 + 동시 에코 → 처리량 측정 |
| F6 | A/B 비교 모드 | P1 | IOCP/RIO 서버 동시 연결, 같은 워크로드 수행, 결과 나란히 비교 |
| F7 | 대형 패킷 테스트 | P2 | 버퍼 사이즈 초과 패킷 전송 테스트 |
| F8 | 세션 로그 뷰 | P2 | 패킷 송수신 로그 실시간 스크롤 |
| F9 | 결과 CSV 내보내기 | P3 | 벤치마크 결과를 파일로 저장 |

### 3.4 기술 스택

| 항목 | 선택 |
|------|------|
| GUI 프레임워크 | Dear ImGui (vcpkg: `imgui[docking-experimental,dx11-binding,win32-binding]`) |
| 차트 라이브러리 | implot (vcpkg: `implot`) |
| 렌더링 백엔드 | DirectX 11 (Windows 전용이므로 최적) |
| 네트워크 | FastPort 엔진 직접 링크 (LibNetworks, LibCommons) |
| 직렬화 | protobuf (기존 Protocols 프로젝트 재사용) |

### 3.5 아키텍처

```
FastPortTestClient.exe
├── main.cpp (ImGui 초기화 + 메인 루프)
├── UI/
│   ├── Dashboard.h/cpp     (메트릭 표시, 차트)
│   ├── Controls.h/cpp      (연결 설정, 테스트 버튼)
│   └── SessionLog.h/cpp    (패킷 로그 뷰)
├── Engine/
│   ├── TestRunner.h/cpp    (에코/플러드/대형패킷 테스트 로직)
│   └── MetricsCollector.h/cpp (카운터 수집 + 통계 계산)
└── Links:
    ├── LibNetworks.lib     (기존 네트워크 엔진)
    ├── LibCommons.lib      (기존 유틸리티)
    └── Protocols.lib       (기존 protobuf 메시지)
```

---

## 4. PRD Sections

### 4.1 Scope

**In scope:**
- ImGui 기반 단일 실행 파일 (FastPortTestClient.exe)
- IOCP/RIO 모드 연결, 에코 테스트, 메트릭 표시
- A/B 비교 화면

**Out of scope:**
- 서버 측 수정 (메트릭 API 추가는 별도 작업)
- 크로스 플랫폼 지원
- 프로덕션 모니터링 도구 (이건 개발/테스트용)

### 4.2 Success Criteria

| # | Criteria | Metric |
|---|---------|--------|
| SC1 | ImGui 창에서 FastPort 서버에 연결 가능 | IOCP/RIO 양 모드 연결 성공 |
| SC2 | 에코 테스트 왕복 시간 실시간 표시 | p50/p95/p99 차트 렌더링 |
| SC3 | 100+ 동시 연결 스케일 테스트 수행 가능 | 처리량 msg/sec 표시 |
| SC4 | A/B 비교 모드에서 IOCP vs RIO 시각적 비교 | 나란히 차트 렌더링 |
| SC5 | 비개발자가 데모를 보고 성능 차이 이해 | 3분 이내 데모 완료 가능 |

### 4.3 Estimated Effort

| Phase | Effort |
|-------|--------|
| ImGui 통합 PoC (빈 창 + DX11) | 2-3시간 |
| F1-F3 (기본 연결 + 에코) | 1일 |
| F4-F5 (차트 + 스케일 테스트) | 1-2일 |
| F6 (A/B 비교) | 1일 |
| F7-F9 (대형 패킷 + 로그 + CSV) | 1일 |
| **Total** | **~1주** |

---

## 5. Next Steps

1. `/pdca plan test-client-ui` — 상세 계획 수립
2. ImGui PoC: `vcpkg add imgui implot` 후 빈 창 렌더링 확인
3. 기존 FastPortClient의 연결 로직을 ImGui 클라이언트로 이식
