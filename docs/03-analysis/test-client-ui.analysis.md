# test-client-ui Gap Analysis Report

> **Feature**: test-client-ui (FastPort ImGui GUI Test Client)
> **Date**: 2026-04-15
> **Phase**: Check (Gap Analysis)
> **Overall Match Rate**: 86.2%

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

## 1. Match Rate Summary

| Category | Score | Status |
|----------|:-----:|:------:|
| Structural Match | 100% | PASS |
| Functional Depth | 88% | PASS |
| Contract/Interface Match | 82% | WARN |
| Intent Match | 90% | PASS |
| Behavioral Completeness | 70% | WARN |
| UX Fidelity | 92% | PASS |
| **Overall (Weighted)** | **86.2%** | **WARN** |

Formula: `(Structural x 0.10) + (Functional x 0.20) + (Contract x 0.20) + (Intent x 0.25) + (Behavioral x 0.15) + (UX x 0.10)`

---

## 2. Plan Success Criteria Status

| # | Criterion | Status | Evidence |
|---|-----------|:------:|---------|
| SC1 | ImGui IOCP 서버 연결 | ✅ Met | TestRunner::Connect() -> IOSocketConnector::Create() |
| SC2 | ImGui RIO 서버 연결 | ⚠️ Partial | 클라이언트는 항상 IOCP로 연결. RIO는 서버 측 차이 (A/B 모드로 커버) |
| SC3 | 에코 RTT 실시간 표시 | ✅ Met | SendEcho -> OnPacketReceived RTT -> MetricsCollector -> UI |
| SC4 | p50/p95/p99 차트 렌더링 | ⚠️ Partial | 텍스트로 표시됨. 차트는 p50만 플롯 (p95/p99 라인 미포함) |
| SC5 | 100개 동시 연결 스케일 | ✅ Met | ConnectScale(ip, port, 100) |
| SC6 | A/B 비교 나란히 차트 | ✅ Met | RenderABComparePanel() 듀얼 차트 |
| SC7 | GUI/IO 스레드 분리 | ✅ Met | IOService worker pool + atomic reads |

**Success Rate: 5/7 Met, 2/7 Partial**

---

## 3. Gap List

### Critical

| # | Gap | Design | Implementation | Note |
|---|-----|--------|----------------|------|
| 1 | RIO 모드 파라미터 | §3.5 Connect(ip, port, bRioMode) | Connect(ip, port) — bRioMode 없음 | IOSocketConnector가 항상 IOCP 사용. 클라이언트 측에서 RIO 소켓 생성 불가 (라이브러리 제약). A/B 모드로 실질적 비교는 가능 |

### Important

| # | Gap | Files | Effort |
|---|-----|-------|--------|
| 2 | 차트에 p95/p99 라인 미표시 | MetricsCollector.ixx, TestClientApp.ixx | 1h |
| 3 | AppState enum 미구현 | TestClientApp.ixx | 1h |
| 4 | IsTestRunning()/StopTest() 미구현 | TestRunner.ixx | 1h |

### Minor

| # | Gap | Files | Effort |
|---|-----|-------|--------|
| 5 | Scale [1000] 버튼 누락 | TestClientApp.ixx | 5min |
| 6 | DX11 디바이스 로스트 복구 | FastPortTestClient.cpp | 1h |
| 7 | 연결 실패 카운터 UI | TestClientApp.ixx, TestRunner.ixx | 30min |

---

## 4. Design Decision Verification

| Decision | Selected | Followed | Note |
|----------|----------|:--------:|------|
| Architecture: Option C (6파일) | Pragmatic | ✅ | 6개 파일 + vcxproj |
| GUI: ImGui + DX11 | ImGui | ✅ | imgui + imgui_impl_dx11 |
| Chart: implot | implot | ✅ | ImPlot::BeginPlot/PlotLine/PlotBars |
| Threading: GUI+IO 분리 | Separate | ✅ | atomic counters, worker pool |
| Metrics: atomic + ring buffer | Lock-free | ✅ | std::atomic + std::array ring |

---

## 5. Strategic Alignment

구현은 핵심 가치 제안(시각적 엔진 검증 + 데모)을 충실히 전달합니다:
- 실시간 메트릭 대시보드 동작
- 에코/스케일/플러드 테스트 기능
- A/B 비교로 IOCP vs RIO 나란히 비교 가능

**주요 전략적 참고**: RIO 모드 연결(Critical #1)은 IOSocketConnector가 소켓을 항상 IOCP 모드로 생성하는 라이브러리 제약입니다. 클라이언트 측 RIO 소켓 지원은 LibNetworks 수정이 필요하여 현재 스코프 밖입니다. A/B 비교 모드가 이를 실질적으로 해결합니다.

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 0.1 | 2026-04-15 | Initial analysis — 86.2% match rate |
