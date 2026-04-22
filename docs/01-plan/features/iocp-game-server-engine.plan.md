# iocp-game-server-engine Planning Document

> **Summary**: RIO 를 freeze 하고 IOCP 한 축에만 집중해, Zero-Copy Recv + Per-Thread Object Pool + KeepAlive + Graceful Shutdown + 재현 가능한 벤치 + 한국어 가이드를 묶은 "IOCP 정석" 게임 서버 엔진 v1 을 산출한다.
>
> **Project**: FastPort
> **Version**: v1 (engine v1 target)
> **Author**: An Younggun
> **Date**: 2026-04-22
> **Status**: Draft

---

## Executive Summary

| Perspective | Content |
|-------------|---------|
| **Problem** | 한국 C++ 게임 서버 개발자가 바로 쓸 수 있는 "IOCP 정석" 오픈 레퍼런스 엔진이 부재하고, 기존 FastPort 도 IOCP/RIO 양쪽을 동시에 밀면서 IOCP 라인의 완성도가 흐려져 있다. |
| **Solution** | RIO 는 v1 범위에서 freeze. IOCP 한 축에 집중해 ① IOSession lifetime race 수정 반영 ② Zero-Copy Recv (`std::span<const std::byte>`) + Per-IOCP-worker Object Pool ③ App ping + TCP keepalive 이중 KeepAlive ④ Graceful Shutdown ⑤ Echo/Chat 샘플 + 재현 가능한 로컬+클라우드 벤치 스크립트 ⑥ 한국어 아키텍처 가이드 v2 를 번들로 산출. |
| **Function/UX Effect** | Recv 경로 atomic/alloc 제거로 P99 tail latency 감소, 세션 장시간 운용시 lifetime race 0 crash, 사용자는 10분 내 Echo 샘플 first-run, 동일 시나리오를 로컬 머신과 클라우드에서 재현 가능. |
| **Core Value** | 64B 기준 **Peak ≥ 30K PPS**, **P50 ≤ 30µs / P99 ≤ 80µs**, stress 1M×2 라운드 **zero crash**, 로컬+클라우드 벤치 재현 문서. 한국어 C++ 게임 서버 중소팀이 "복붙해도 되는" 레퍼런스 확보. |

---

## Context Anchor

> Auto-generated. Propagates to Design/Do documents for context continuity.

| Key | Value |
|-----|-------|
| **WHY** | IOCP 정석 오픈 레퍼런스 부재 + FastPort 의 IOCP 라인 완성도 확보 (RIO 양다리 제거) |
| **WHO** | 한국 C++ 1~5인 인디/미드코어 서버팀 (CCU 1K~20K). Windows, MSVC, C++20, IOCP 경험 보유 |
| **RISK** | ① `std::span` 수명 오해로 인한 use-after-return ② lifetime race edge-case 재발 ③ 벤치 재현성 (환경 고지 없는 수치) |
| **SUCCESS** | Peak ≥ 30K PPS@64B / P50 ≤ 30µs / P99 ≤ 80µs / stress 1M×2 zero crash / Echo 샘플 10분 first-run / 로컬+클라우드 동일 벤치 결과 포함 |
| **SCOPE** | M1 lifetime race 반영, M2 Zero-Copy Recv + Per-Thread Pool, M3 KeepAlive(App+TCP), M4 Graceful Shutdown, M5 Echo/Chat 샘플 + 벤치, M6 한국어 가이드 v2. RIO/Linux/멀티노드/게임로직 OOS |

---

## 1. Overview

### 1.1 Purpose

IOCP 기반 C++ 게임 서버 엔진을 **최적화 완료 상태로 v1 릴리즈**한다. 이 엔진은 FastPort 프로젝트의 `LibNetworks` + `FastPortServer` 조합으로 제공되며, 동일 프로젝트의 `LibNetworksRIO` / `FastPortServerRIO` 는 본 v1 기간 동안 freeze (유지보수만, 신규 기능 없음).

### 1.2 Background

- 최근 작업(PRD `iocp-game-server-engine.prd.md`)에서 IOCP 축은 **Scatter-Gather Send(+45%)**, **Zero-Copy Send (P50 31µs / Peak 30.97K PPS)**, **Session Idle Timeout**, **Admin ServerStatus**, **RIO 코드 분리** 까지 완료됨.
- 남은 갭은 **Recv 경로 Zero-Copy**, **Object Pool 일원화**, **IOSession lifetime race 수정 흡수**, **KeepAlive 표준화**, **Graceful Shutdown**, **재현 가능한 샘플/벤치**, **한국어 문서**.
- 현재 벤치 결과 파일(`benchmark-results-01-baseline.md`, `-02-scatter-gather.md`, `-03-zero-copy-send.md`)은 환경 고지가 부족하고 v1 스펙 변경 이후 수치가 바뀌므로 **전량 폐기**하고 로컬 필수 + 클라우드 배포 벤치로 재생산 예정.

### 1.3 Related Documents

- PRD: `docs/00-pm/iocp-game-server-engine.prd.md`
- 선행 Plan/Design: `docs/01-plan/features/iosession-lifetime-race.plan.md`, `docs/02-design/features/iosession-lifetime-race.design.md` (M1 은 여기를 참조)
- 기존 아키텍처: `docs/ARCHITECTURE_IOCP.md` (→ v2 로 갱신 예정)
- 프로토콜: `docs/PACKET_PROTOCOL.md`
- 기존 벤치: `docs/benchmark-results-01-baseline.md`, `-02-scatter-gather.md`, `-03-zero-copy-send.md` (**v1 에서 폐기 예정**)

---

## 2. Scope

### 2.1 In Scope

- [ ] **M1 — IOSession lifetime race 수정 반영**: 별도 Plan/Design 에 따라 구현 중인 Option C Pragmatic 안이 본 v1 에 **병합 완료**되어야 함. 본 Plan 은 종속성만 선언하고 설계/구현은 기존 문서에 위임.
- [ ] **M2 — Zero-Copy Recv + Per-Thread Object Pool**:
  - `PacketFramer` 가 `std::span<const std::byte>` 기반 콜백(`OnPacket(SessionId, std::span<const std::byte>)`) 전달
  - Recv WSABUF 를 Object Pool 에서 할당 (기본 할당자 제거)
  - Pool 전략: **Per-IOCP-worker (TLS) Pool** + worker 간 이동된 buffer 반환 처리 (원 소속 worker 로 deferred return 또는 crossing-aware free-list)
  - 비동기 보관이 필요한 사용자를 위해 `Buffer::CopyToHeap()` / `Buffer::ToShared()` 명시 헬퍼 제공
- [ ] **M3 — KeepAlive (App + TCP 이중)**:
  - App-level ping/pong: idle 5s → ping, 1s 내 pong 없으면 재시도 3회 후 close (값 전부 tunable)
  - TCP-level SO_KEEPALIVE / `SIO_KEEPALIVE_VALS` 설정 옵션 제공
  - 두 레이어를 `KeepAliveConfig` 단일 구조체로 노출
- [ ] **M4 — Graceful Shutdown**:
  - Accept 중단 → in-flight IO drain(timeout 포함) → worker thread join → TimerQueue/Logger shutdown 순서 고정
  - 기존 CLAUDE.md "종료 순서 주의" 규칙 준수 (TimerQueue before Logger)
- [ ] **M5 — Echo / Chat 샘플 + 재현 가능 벤치 스크립트**:
  - `samples/echo`, `samples/chat` 신규 (또는 FastPortServer 내 sample 서브디렉토리)
  - `benchmark/` 스크립트: 로컬 머신 필수 실행 + 클라우드(AWS/Azure 중 1택) 배포 실행 모두 문서화
  - **기존 `docs/benchmark-results-*.md` 3개 삭제**, v1 결과로 교체
- [ ] **M6 — 한국어 아키텍처 가이드 v2**:
  - `docs/ARCHITECTURE_IOCP.md` → v2 로 재작성 (Zero-Copy Recv 흐름도, Per-Thread Pool 수명 규칙, KeepAlive 이중 레이어, Graceful Shutdown 순서)
  - `docs/ko/` 하위에 한국어 본문 유지

### 2.2 Out of Scope

- RIO 신규 기능 일체 (`LibNetworksRIO`, `FastPortServerRIO`, `LibNetworksRIOTests` 은 **freeze**, 빌드 유지용 bug fix 만 허용)
- Linux/epoll 이식, io_uring
- 멀티 노드 클러스터링 / 게이트웨이 / 샤딩
- 실제 게임 로직 레이어 (MMO tick, interest management 등) — 엔진/네트워크만
- GUI 툴, Prometheus/Grafana 통합 (Admin ServerStatus 는 이미 존재, 이 Plan 에서 신규 확장 없음)
- 인증/암호화 (TLS) 레이어 — v1.1 이상

---

## 3. Requirements

### 3.1 Functional Requirements

| ID | Requirement | Priority | Status |
|----|-------------|----------|--------|
| FR-01 | IOSession lifetime race 수정이 main 에 병합되고, stress reproducer 1M×2 라운드에서 crash/leak 0 | High | Pending (별도 feature 진행 중) |
| FR-02 | `PacketFramer::OnPacket` 콜백이 `std::span<const std::byte>` 로 전달되며 heap alloc/memcpy 가 per-packet 경로에 없음 | High | Pending |
| FR-03 | Recv WSABUF 가 Per-IOCP-worker TLS Object Pool 에서 공급되고, crossing-worker return 이 안전하게 처리됨 | High | Pending |
| FR-04 | `Buffer::CopyToHeap()` / `Buffer::ToShared()` 헬퍼로 비동기 보관 경로가 1줄 API 로 제공됨 | High | Pending |
| FR-05 | `KeepAliveConfig { bool appPingEnabled; ms idle; ms pongTimeout; int maxRetries; bool tcpKeepAliveEnabled; ... }` 단일 진입점으로 양쪽 레이어가 설정됨 | High | Pending |
| FR-06 | Graceful Shutdown 호출 시 accept 중단 → in-flight drain(timeout) → worker join → TimerQueue shutdown → Logger shutdown 순서가 보장되고, 잔존 callback 이 닫힌 Logger 를 참조하지 않음 | High | Pending |
| FR-07 | Echo 샘플이 신규 사용자 환경에서 clone→build→run 10분 이내 first-packet echo 성공 | High | Pending |
| FR-08 | 재현 가능한 벤치 스크립트: 동일 스크립트가 로컬 머신 + 지정된 클라우드 인스턴스에서 모두 실행되고, 결과 markdown 이 환경 정보(OS, CPU, NIC, RTT, 코어수, Windows build, 컴파일러, 옵션)를 포함 | High | Pending |
| FR-09 | 기존 `docs/benchmark-results-0{1,2,3}*.md` 삭제 후 v1 결과 1세트로 교체 | Medium | Pending |
| FR-10 | `docs/ARCHITECTURE_IOCP.md` v2: 4가지 다이어그램(Recv flow, Pool lifetime, KeepAlive, Shutdown) 포함, 한국어 | Medium | Pending |
| FR-11 | RIO 관련 프로젝트는 기존 상태 유지(빌드만 성공) — 기능 추가/변경 PR 은 본 v1 에서 거절 | Medium | Pending |

### 3.2 Non-Functional Requirements

| Category | Criteria | Measurement Method |
|----------|----------|-------------------|
| Performance — Throughput | 64B echo: Peak ≥ 30,000 PPS per server process (single host) | `benchmark/bench_echo.ps1` 결과 markdown |
| Performance — Latency | 64B echo: P50 ≤ 30µs, P99 ≤ 80µs (LAN, local client+server) | 동일 벤치 스크립트, hdrhistogram or custom latency log |
| Reliability | Stress 1M packet × 2 round, zero crash / zero leak / zero SetEvent on dangling handle | `LibNetworksTests` 기존 stress + 신규 lifetime race reproducer, AddressSanitizer off (MSVC), `/RTC1` Debug build |
| Memory | Recv 경로 steady-state 0 alloc (Pool 완전 hit) | Pool hit/miss 카운터 + Admin ServerStatus |
| Compatibility | Windows 10 1809+ / Windows Server 2019+, MSVC 14.38+, C++20, x64 only | CI matrix (없으면 로컬 2환경 수동) |
| Reproducibility | 동일 벤치 스크립트가 로컬/클라우드에서 실행되고 결과 파일에 환경 블록 포함 | `bench-env.md` 섹션 템플릿 강제 |
| Logging | 모든 로그 `LibCommons::Logger`, spdlog 직접 호출 금지 (CLAUDE.md 준수) | grep PR 체크 |

---

## 4. Success Criteria

### 4.1 Definition of Done

- [ ] **SC-1 (Peak PPS)** 64B echo Peak ≥ 30,000 PPS 로컬 환경 기록
- [ ] **SC-2 (Latency)** P50 ≤ 30µs, P99 ≤ 80µs 로컬 환경 기록
- [ ] **SC-3 (Stress)** lifetime race reproducer 1M×2 라운드 crash=0 leak=0
- [ ] **SC-4 (Pool hit)** steady-state Recv alloc=0 (Admin counter 로 관측)
- [ ] **SC-5 (First-run)** 신규 사용자 Echo 샘플 10분 이내 동작 (리드미 따라가기 timed run 으로 검증)
- [ ] **SC-6 (Cloud repro)** 클라우드 인스턴스에서 동일 스크립트로 벤치 실행 후 결과 markdown 병합
- [ ] **SC-7 (Docs)** `ARCHITECTURE_IOCP.md` v2 + 4 다이어그램 + 한국어
- [ ] **SC-8 (Sample)** `samples/echo`, `samples/chat` 빌드/실행 OK, README 포함
- [ ] **SC-9 (RIO freeze 준수)** 본 v1 기간 동안 RIO 관련 신규 PR 0건

### 4.2 Quality Criteria

- [ ] Zero lint warning (MSVC `/W4` 목표, 최소 `/W3` + `/WX` 회귀 금지)
- [ ] `LibNetworksTests` / `LibCommonsTests` 전부 green
- [ ] Debug `/RTC1` 빌드에서 stress reproducer 통과 (스택 오염 검출 0건)
- [ ] `LibCommons::Logger` 표준 준수 — spdlog 직접 호출 0건 (grep 검증)
- [ ] 모듈 GMF 에 `#include <spdlog/spdlog.h>` 규약 준수 (Logger 쓰는 `.cpp` 전수)

---

## 5. Risks and Mitigation

| Risk | Impact | Likelihood | Mitigation |
|------|--------|------------|------------|
| **R1. `std::span` 수명 오해로 use-after-return** | High (crash, 데이터 손상) | Medium | ① 콜백 시그니처 docstring 필수 ② `Buffer::CopyToHeap()` / `ToShared()` 명시 API ③ `ARCHITECTURE_IOCP.md` 에 "보관 금지" 경고 + 예시 ④ 디버그 빌드에서 span underlying buffer magic cookie 로 use-after-free 탐지 |
| **R2. IOSession lifetime race edge-case 재발** | High (crash, TimerQueue dangling) | Medium | ① 기존 Plan+Design(Option C Pragmatic) 병합 대기 ② stress reproducer 케이스 확장 (concurrent close×recv, close×send, close×timer) ③ CLAUDE.md timer callback lifetime 규칙 전수 grep |
| **R3. Per-Thread Pool 의 worker crossing** | Medium (누수 or lock contention) | Medium | ① crossing return queue(원 소속 TLS 로 deferred) 로 시작 ② 측정 후 필요시 lock-free global fallback ③ Admin 에 crossing counter 노출 |
| **R4. 벤치 재현성 (수치 편차)** | Medium (문서 신뢰도) | High | ① 환경 블록 템플릿 강제 (CPU model, nominal GHz, P/E core ratio, NIC driver, Windows build, 전원 플랜, timer resolution, MSVC 버전, cl opts) ② 동일 스크립트 로컬+클라우드 2세트 게시 ③ 수치는 range (p50/p99/min/max) + N run 으로 표기 |
| **R5. 기존 벤치 결과 삭제로 인한 referential 손실** | Low (과거 기록이 이슈에서 링크됨) | Medium | 삭제 전 git commit 에 "archived in commit <sha>" 남기고, 필요시 `docs/archive/benchmarks-v0/` 로 이동 (삭제 대신 이동 권장) |
| **R6. MSVC C++20 모듈 ICE 재발 (Logger)** | Medium (빌드 중단) | Low | CLAUDE.md 규칙 준수 — Logger 사용하는 모듈 구현 `.cpp` 의 GMF 에 반드시 `#include <spdlog/spdlog.h>` |
| **R7. Graceful Shutdown 순서 역전** | High (segfault, CLAUDE.md 사례) | Medium | 순서 고정 단일 함수 `ServerLifecycle::Shutdown()` 만 공개, 내부 순서 유닛 테스트로 잠금 |
| **R8. RIO freeze 규칙 유실** | Low (개발자가 실수로 RIO 확장) | Medium | `CODEOWNERS` 또는 PR template 에 "RIO freeze for v1" 체크박스, 본 Plan doc 을 프로젝트 루트 README 에 링크 |

---

## 6. Impact Analysis

### 6.1 Changed Resources

| Resource | Type | Change Description |
|----------|------|--------------------|
| `LibNetworks::PacketFramer` | Module / API | OnPacket 콜백 시그니처 변경: `(SessionId, std::span<const std::byte>)` |
| `LibNetworks::IOSession` | Module / Internal | Recv buffer 공급원을 Pool 로 교체, lifetime race 수정 병합 |
| `LibNetworks::Buffer` (또는 신규) | Module / API | `CopyToHeap()`, `ToShared()` 신규, span 어댑터 |
| `LibNetworks::RecvBufferPool` | 신규 모듈 | Per-IOCP-worker TLS pool + crossing return queue |
| `LibNetworks::KeepAliveConfig` | 신규 API | App ping + TCP keepalive 통합 설정 |
| `LibNetworks::ServerLifecycle` | 모듈/API | Graceful Shutdown 순서 고정 진입점 |
| `FastPortServer` | 실행형 | 위 API 사용 이관 + 샘플 2개(Echo/Chat) 추가 |
| `docs/ARCHITECTURE_IOCP.md` | 문서 | v2 재작성 (한국어) |
| `docs/benchmark-results-*.md` (3개) | 문서 | **폐기** (또는 `docs/archive/benchmarks-v0/` 이동) |
| `benchmark/` | 스크립트 | 로컬+클라우드 벤치 스크립트 신규/갱신 |
| `samples/echo`, `samples/chat` | 실행형 | 신규 |
| `LibNetworksRIO` / `FastPortServerRIO` | 프로젝트 | **변경 없음** (freeze) |

### 6.2 Current Consumers

| Resource | Operation | Code Path | Impact |
|----------|-----------|-----------|--------|
| `PacketFramer::OnPacket` | READ (callback 주입) | `FastPortServer` 패킷 핸들러, `FastPortTestClient` 시뮬레이터, `LibNetworksTests` 단위 테스트 | **Breaking (시그니처 변경)** — 전 호출부 span 으로 이관 필요 |
| `IOSession::RecvComplete` | UPDATE (버퍼 공급 경로) | `LibNetworks` 내부 only | Internal only, API 불변 |
| `IOSession::Close/Shutdown` | UPDATE | `FastPortServer` shutdown path | API 이름/의미 그대로 유지, 내부 race 수정 흡수 |
| SessionIdleTimeout API | READ | 기존 사용자 (이 Plan 에서 유지) | None — KeepAlive 와 **별개 레이어** 로 공존 |
| Admin ServerStatus | READ | 외부 admin 툴/대시보드 | Additive — Pool hit/miss counter, KeepAlive stats 추가 (기존 필드 불변) |
| `LibNetworksRIO::*` | 전부 | `FastPortServerRIO` | **의도적으로 변경 금지** |

### 6.3 Verification

- [ ] `PacketFramer::OnPacket` span 이관 후 모든 consumer 컴파일 OK (FastPortServer, FastPortTestClient, LibNetworksTests)
- [ ] SessionIdleTimeout 기존 테스트 green
- [ ] Admin ServerStatus 기존 필드 응답 shape 변경 없음 (additive only)
- [ ] RIO 관련 테스트/실행 회귀 없음 (빌드 + 기존 테스트 green)
- [ ] Graceful Shutdown 순서 유닛 테스트 (TimerQueue → Logger 순 잠금)
- [ ] lifetime race reproducer 1M×2 라운드 green

---

## 7. Architecture Considerations

### 7.1 Project Level Selection

| Level | Characteristics | Recommended For | Selected |
|-------|-----------------|-----------------|:--------:|
| **Starter** | 단순 구조 | 정적 사이트 | ☐ |
| **Dynamic** | Feature-based, BaaS | SaaS/웹앱 | ☐ |
| **Enterprise** | 엄격한 레이어 분리, DI, 네이티브 성능/재현성 중시 | 고성능 시스템, C++ 네트워크 엔진 | ☑ |

> FastPort 는 기존부터 C++ 네이티브 네트워크 엔진이며 Enterprise 레벨로 유지. bkit 의 웹 중심 컨벤션(React/Next.js 등) 은 적용 대상 아님.

### 7.2 Key Architectural Decisions

| Decision | Options | Selected | Rationale |
|----------|---------|----------|-----------|
| 언어 표준 | C++20 / C++23 | **C++20 (`/std:c++20`)** | CLAUDE.md 기록 — C++23 상향 시 FastPortServer 내부 C1001 ICE 재현. C++23 기능은 독립 모듈 상향 가능 시점까지 보류 |
| 플랫폼 | Win32 / x64 | **x64 only** | Commons.props 에 기록, 실성능 타겟 |
| 네트워크 프리미티브 | IOCP / RIO / 둘 다 | **IOCP 전용 v1** | PRD & 사용자 지시. RIO 는 freeze |
| Recv 버퍼 소유권 | span / shared_ptr / hybrid | **`std::span<const std::byte>` + 명시 copy 헬퍼** | 최적 성능 — atomic inc/dec 제거, alloc 제거, P99 80µs 타깃 기여. 비동기 보관은 `CopyToHeap()`/`ToShared()` 로 명시 |
| Recv 버퍼 풀 | Per-session / Per-thread / Global lock-free | **Per-IOCP-worker TLS + crossing return** | 캐시 친화 + contention 제거. worker 간 이동 버퍼는 원 소속 TLS deferred return |
| KeepAlive | App / TCP / 둘 다 | **App ping + TCP keepalive 옵션 (이중)** | 게임 서버 반응성 확보 + TCP 레벨 보조 |
| 로거 | spdlog 직접 / LibCommons::Logger | **`LibCommons::Logger`** | CLAUDE.md 프로젝트 표준 |
| 뮤텍스 | std::mutex / custom | **`std::mutex` + `std::lock_guard` / `LibCommons::RWLock`** | CLAUDE.md 컨벤션 |
| 테스트 | 기존 `LibCommonsTests`, `LibNetworksTests` 유지 | 동일 | MSVC Test Framework, 기존 유지 |
| 벤치 | ad-hoc / scripted | **scripted (`benchmark/bench_echo.*` 스크립트 + 환경 블록 강제)** | 재현성 요구 |
| 벤치 환경 | 로컬만 / 클라우드만 / 둘 다 | **로컬 필수 + 클라우드 배포 병행** | 사용자 지시 |

### 7.3 Clean Architecture Approach

```
Selected Level: Enterprise (C++ 네트워크 엔진 프로필)

Project Structure (current → v1 변경 없음, 내부 모듈만 추가):
┌─────────────────────────────────────────────────────┐
│ Solution Root/                                      │
│   LibCommons/          (Logger, TimerQueue, RWLock) │
│   LibNetworks/         (IOCP 축 — v1 집중)          │
│     + RecvBufferPool   (신규)                       │
│     + KeepAliveConfig  (신규)                       │
│     + ServerLifecycle  (Graceful Shutdown)          │
│     + Buffer helpers   (CopyToHeap / ToShared)      │
│   LibNetworksRIO/      (freeze)                     │
│   FastPortServer/      (IOCP 실행형 + samples 참조) │
│   FastPortServerRIO/   (freeze)                     │
│   FastPortClient/                                   │
│   FastPortTestClient/                               │
│   FastPortBenchmark/                                │
│   Protocols/ Protos/                                │
│   samples/echo/        (신규)                       │
│   samples/chat/        (신규)                       │
│   benchmark/           (스크립트 + env 템플릿)      │
│   docs/                                             │
└─────────────────────────────────────────────────────┘
```

---

## 8. Convention Prerequisites

### 8.1 Existing Project Conventions

- [x] `CLAUDE.md` coding conventions 존재 — Logger/Timer callback lifetime/C++ std/네이밍 명시
- [ ] `docs/01-plan/conventions.md` — 없음 (Phase 2 문서 미생성, 현재 CLAUDE.md 로 갈음)
- [ ] `CONVENTIONS.md` — 없음
- [ ] ESLint — N/A (C++)
- [ ] Prettier — N/A (C++)
- [x] `Commons.props` — `/std:c++20`, x64 설정
- [x] `Application.props` — 공용 애플리케이션 설정
- [x] `vcpkg.json` / `vcpkg-configuration.json` — 서드파티 의존성

### 8.2 Conventions to Define/Verify

| Category | Current State | To Define | Priority |
|----------|---------------|-----------|:--------:|
| **Naming** | CLAUDE.md 에 정의 (PascalCase/m_/k 접두/`::` Win32) | 유지 | — |
| **Module 컨벤션** | CLAUDE.md — `commons.snake_case`, GMF 순서 | 유지 + Recv Pool 모듈명 확정 (`commons.recv_buffer_pool` 또는 `networks.recv_pool`) | High |
| **Logger 사용** | CLAUDE.md — GMF 에 spdlog 포함 의무 | 유지, 신규 모듈 전부 준수 | High |
| **Timer callback lifetime** | CLAUDE.md — periodic ref 캡처 금지 | 유지, KeepAlive 타이머에 그대로 적용 | High |
| **Shutdown 순서** | CLAUDE.md — TimerQueue before Logger | 명문화된 `ServerLifecycle::Shutdown()` 1진입점으로 고정 | High |
| **벤치 환경 블록** | 미정 | `benchmark/ENV_TEMPLATE.md` 신규 — markdown 결과 파일 상단 필수 블록 | High |
| **샘플 README** | 미정 | `samples/*/README.md` 필수 섹션(Prerequisites, Build, Run, Expected output, Troubleshooting) | Medium |

### 8.3 Environment Variables / Build Flags

| Variable | Purpose | Scope | To Be Created |
|----------|---------|-------|:-------------:|
| `FASTPORT_LOG_DIR` | Logger 로그 파일 경로 override | Runtime | ☐ (선택) |
| `FASTPORT_BENCH_ENV` | 벤치 스크립트가 기록할 환경 이름 (local/cloud-aws-c6i/…) | Build/Runtime | ☑ |
| `FASTPORT_BENCH_CLOUD_PROVIDER` | 벤치 배포 대상 (aws/azure) | CI/Scripts | ☑ |

> **Web 쪽 env 템플릿(NEXT_PUBLIC_API_URL 등)은 본 프로젝트에 N/A.**

### 8.4 Pipeline Integration

> bkit 9-phase 파이프라인은 웹 중심이라 본 C++ 엔진 플랜에는 직접 적용하지 않는다. 필요한 요소만 발췌:

| Phase | Status | Document Location | Note |
|-------|:------:|-------------------|------|
| Phase 1 (Schema) | N/A | — | C++ 엔진, 스키마 레벨 개념 대신 `docs/PACKET_PROTOCOL.md` 가 역할 담당 |
| Phase 2 (Convention) | 부분 | `CLAUDE.md` | 별도 `docs/01-plan/conventions.md` 는 v1 내 불필요 |

---

## 9. Next Steps

1. [ ] M1 (IOSession lifetime race) 별도 feature Plan/Design 병합 대기 — 본 feature 종속성만 선언
2. [ ] `/pdca design iocp-game-server-engine` — 3가지 아키텍처 옵션 생성 후 선택 (M2 의 span/pool 구조, M3 KeepAlive 배치, M4 Shutdown state machine 중심)
3. [ ] Design 승인 후 `/pdca do iocp-game-server-engine --scope M2` 부터 모듈별 구현
4. [ ] 모듈 완료마다 `/pdca analyze` → 필요시 `/pdca iterate`
5. [ ] 전체 M2~M6 구현 완료 후 `/pdca qa iocp-game-server-engine` → `/pdca report`

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-04-22 | Initial draft (PRD 기반, RIO freeze, M1~M6 스코프, Recv=span+Per-Thread Pool, App+TCP KeepAlive, 로컬+클라우드 벤치) | An Younggun |
