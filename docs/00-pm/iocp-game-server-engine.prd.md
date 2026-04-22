# iocp-game-server-engine - Product Requirements Document

> **Date**: 2026-04-22
> **Author**: AnYounggun (boinred.dev@gmail.com)
> **Method**: bkit PM Agent Team (pm-lead 종합 분석 — Discovery / Strategy / Research / PRD)
> **Status**: Draft v1 — 전략적 방향: IOCP 우선, RIO 보류
> **Scope Signal (from user)**: "우선 RIO 기능은 잠시 뒤로 미루고 IOCP 부터 최적화 완료한 게임 서버 엔진을 만드는 것이 목표야."

---

## Executive Summary

| Perspective | Content |
|-------------|---------|
| **Problem** | 한국 Windows 게임 서버 개발자가 "IOCP를 올바르게 쓰는" 오픈 엔진을 찾기 힘들다. 대부분의 자료는 토이 수준이거나 상용 미들웨어(Photon/프로퍼게이트) 블랙박스이고, 수명 레이스·메모리 할당 지터·스캐터-개더 송신·idle 감지 같은 실전 이슈가 한 묶음으로 해결된 레퍼런스가 없다. |
| **Solution** | **IOCP 전용 게임 서버 엔진 v1**. Scatter-Gather WSASend + Zero-Copy Send + Session Idle Timeout + IOSession Lifetime Race 수정이 이미 라이브러리에 내장되어 있는 상태에서, 수신 경로 Zero-Copy, 패킷 오브젝트 풀, 공격적 Keep-Alive 튜닝, Admin 채널까지 포함한 "turnkey" 수준으로 마감한다. RIO는 v2로 분리. |
| **Target User** | 1차(Beachhead): **한국 1–5인 C++ 인디/미드코어 게임 서버 팀** (동시접속 1K–20K, Windows 데디 운영, MMO/RPG/SLG 서브시스템). 2차: C++ IOCP 학습·자료 수요의 시니어 개발자. |
| **Core Value** | "IOCP의 정석 레퍼런스 + 실전 즉시 투입 가능": **P50 ≤ 30µs, P99 ≤ 80µs, 30K+ PPS(64B)** 성능을 한국어 문서와 함께 제공하며, RIO 분리·테스트 자산·벤치마크 재현 가능성을 보증한다. |

---

## Table of Contents

1. Phase 1 — Discovery (pm-discovery)
2. Phase 2 — Strategy (pm-strategy)
3. Phase 3 — Research (pm-research)
4. Phase 4 — Beachhead & GTM (pm-prd)
5. Phase 5 — PRD 8-Section
6. Phase 6 — Pre-mortem · User Stories · Test Scenarios
7. Phase 7 — Stakeholder Map · Attribution

---

## 1. Phase 1 — Discovery (Opportunity Solution Tree)

### 1.1 5-Step Discovery Chain

**Step 1 — Brainstorm (pain points for an IOCP game-server engine maintainer / user)**

| # | 아이디어 | Category |
|---|---------|----------|
| 1 | WSASend에서 임시 복사 제거 (Scatter-Gather) | Perf (✅ 완료) |
| 2 | 직렬화 시 링버퍼에 직접 write (Zero-Copy Send) | Perf (✅ 완료) |
| 3 | Zero-Copy **Recv** (PacketFramer가 span 반환) | Perf (진행 필요) |
| 4 | Packet / OverlappedEx Object Pool | Perf/Jitter |
| 5 | Session Idle Timeout (TimerQueue 기반) | Correctness (✅ 완료) |
| 6 | IOSession lifetime race (use-after-free) 수정 | Correctness (🟡 Plan/Design 완료, 구현중) |
| 7 | TCP Keep-Alive 공격적 튜닝 (5s/1s/3) | Correctness |
| 8 | Admin 채널 (ServerStatus 등) | Ops (✅ 초판 완료) |
| 9 | 구조화 로깅 + 카테고리별 레벨 핫리로드 | Ops |
| 10 | Pre-posted AcceptEx 수 튜닝 가이드 | Perf/Tuning |
| 11 | 패킷 스키마/버전 네고시에이션 | Protocol |
| 12 | 한국어 아키텍처 가이드 + 벤치마크 재현 스크립트 | DX / GTM |
| 13 | FastPortBenchmark = 공식 성능 리포트 도구화 | Trust |
| 14 | 최소 예제 (Echo / Chat / Room) | Onboarding |
| 15 | 우아한 종료 / graceful shutdown 흐름 | Correctness |

**Step 2 — Assumptions (가장 위험한 가정)**

| # | Assumption | 근거 | Risk |
|---|-----------|------|------|
| A1 | "한국 C++ IOCP 게임 서버 개발자"는 이 엔진을 실제로 도입할 의향이 있다 | 시장 규모 가설 | High |
| A2 | P50 30µs / P99 80µs / 30K PPS 는 현재 구현으로 달성 가능하다 | Baseline 20K→30.97K PPS 이미 달성 | Medium |
| A3 | RIO를 뒤로 미루어도 성능 경쟁력이 충분하다 (대다수 유스케이스에서) | 대부분 게임 서버 CCU/PPS에서 IOCP로 충분 | Medium |
| A4 | IOSession lifetime race 가 Option C Pragmatic 수정으로 완전 해결된다 | 이미 Plan+Design 완료, Stress reproducer 있음 | Medium-High |
| A5 | MSVC C++20 모듈이 실전 게임 프로젝트에서 지속 가능하다 (CI/빌드 시간/ICE) | 이미 프로젝트에서 ICE 대응 가이드 축적 | Medium |
| A6 | 사용자는 "샘플 + 벤치마크 재현" 을 보면 신뢰한다 | 실전 인프라 구매 패턴 | Low-Medium |

**Step 3 — Prioritize (Impact × Risk Matrix)**

| Assumption | Impact | Risk | Priority |
|-----------|--------|------|----------|
| A1 수요 존재 | 10 | 9 | **P0** |
| A4 lifetime race 해결 | 9 | 7 | **P0** |
| A2 성능 목표 | 8 | 5 | P1 |
| A3 RIO 없이도 충분 | 8 | 5 | P1 |
| A5 모듈 지속성 | 6 | 5 | P2 |
| A6 신뢰 경로 | 7 | 3 | P2 |

**Step 4 — Experiments**

- A1: 깃허브 README 를 실전 레퍼런스 포지셔닝으로 리라이트 후 한국 C++ 개발자 커뮤니티(KGC 슬랙, C++ Korea, 개인 블로그)에서 피드백 수집
- A4: Stress reproducer가 1M 반복에서 0 crash 로 통과하는지 CI 에 포함
- A2: FastPortBenchmark 로컬 + 원격 머신 2종에서 재현 + 결과 md 에 환경 기록 의무화
- A3: 벤치마크 비교표(IOCP v1 vs RIO 참고치 vs Photon Server 공식 수치) 작성

**Step 5 — Opportunity Solution Tree**

```
Outcome: IOCP 기반 오픈 게임 서버 엔진으로 "실전에서 바로 쓸 수 있다"는 신뢰 확보
├── Opportunity 1: Send 경로 지터/복사 제거  (✅ 대부분 완료)
│   ├── Solution 1A: Scatter-Gather WSASend            ✅ Done
│   └── Solution 1B: Zero-Copy Send (링버퍼 직접 직렬화) ✅ Done
├── Opportunity 2: Recv 경로 할당/복사 제거  (🔴 v1 핵심 과제)
│   ├── Solution 2A: PacketFramer → span 반환 Zero-Copy Recv
│   ├── Solution 2B: Packet / OverlappedEx Object Pool
│   └── Solution 2C: OnPacketReceived 핸들러에 span 전달 + 수명 규약
├── Opportunity 3: Correctness (수명/idle/graceful)  (🟡 일부 진행)
│   ├── Solution 3A: IOSession Lifetime Race 수정 완료   (Plan+Design 있음)
│   ├── Solution 3B: Session Idle Timeout                 ✅ Done
│   ├── Solution 3C: 공격적 Keep-Alive 튜닝 가이드 + API
│   └── Solution 3D: Graceful Shutdown (리스너 stop → drain → 세션 close)
├── Opportunity 4: 운영 가시성
│   ├── Solution 4A: Admin 채널 (ServerStatus)            ✅ Done
│   ├── Solution 4B: 카테고리 로그 레벨 동적 변경
│   └── Solution 4C: Per-session counters (RX/TX bytes, last activity)
└── Opportunity 5: 개발자 신뢰 / GTM
    ├── Solution 5A: 한국어 아키텍처 가이드 (ARCHITECTURE_IOCP.md 확장)
    ├── Solution 5B: 벤치마크 재현 스크립트 + 환경 기록 템플릿
    ├── Solution 5C: 최소 예제 (Echo, Chat, Room) 서버
    └── Solution 5D: "IOCP 정석" 블로그 시리즈 (5편) + KGC / C++Korea 노출
```

**v1 Scope → Opportunity 2 + 3 + (일부) 5** 를 IOCP 엔진 완성도의 필수 조건으로 묶는다.

---

## 2. Phase 2 — Strategy

### 2.1 JTBD 6-Part Value Proposition

| 구분 | 내용 |
|------|------|
| **For** (대상 고객) | Windows 기반 C++ 게임 서버를 직접 만드는 한국 1–5인 인디/미드코어 팀 |
| **Who** (현재 느끼는 불편) | IOCP 를 올바르게 쓴 오픈 레퍼런스가 없고, 상용 솔루션(Photon 등)은 비용·블랙박스·커스터마이즈 제약이 크다 |
| **Our Product** (FastPort IOCP Engine v1) | Scatter-Gather + Zero-Copy Send/Recv + 세션 수명 안전성까지 해결된 Windows IOCP 게임 서버 엔진 |
| **That** (제공 가치) | 30µs P50 / 30K+ PPS 수준의 성능을 한국어 아키텍처 문서와 재현 가능한 벤치마크로 제공 |
| **Unlike** (경쟁 대안) | Photon(유료·블랙박스), Mirror(Unity 종속·TCP 소켓 직), 자체 구현(수명 레이스 지뢰), OSS IOCP 예제(토이 수준) |
| **Our Product** (재강조) | FastPort 는 프로덕션 이슈(lifetime race, idle, 지터)를 먼저 맞이한 엔진이다 |

### 2.2 Lean Canvas

| Section | Content |
|---------|---------|
| **1. Problem** | (1) IOCP 실전 레퍼런스 부재 (2) RIO 를 섞으면 복잡도 폭발 (3) 수명·지터·idle 같은 프로덕션 버그를 혼자 맞닥뜨림 |
| **2. Customer Segments** | Early adopter: 한국 C++ 인디 게임 서버 개발자 / 확장: 시니어 C++ 네트워크 엔지니어 / 교육용 |
| **3. Unique Value Proposition** | "IOCP의 정석을 한국어로 제공하는, 이미 5건의 RIO 안정성 이슈를 맞고 돌아온 엔진" |
| **4. Solution** | IOCP 코어 + Session/Listener/Connector + Zero-Copy 경로 + Idle/Keep-Alive + Admin 채널 + 샘플 |
| **5. Channels** | GitHub README + 한국어 블로그 시리즈 + KGC/C++ Korea 커뮤니티 + 벤치마크 리포트 공개 |
| **6. Revenue Streams** | (v1 범위 밖) OSS 무료, 향후 컨설팅 / 커스텀 포팅 / 교육 |
| **7. Cost Structure** | 본인 개발 시간, GitHub 호스팅, 벤치마크 하드웨어(개인 워크스테이션) |
| **8. Key Metrics** | GitHub Stars, README 유입, 샘플 clone 수, 벤치마크 md 재인용 수, Issue/PR 수 |
| **9. Unfair Advantage** | 이미 확보한 실측 벤치마크(20K→30.97K PPS, P50 31µs) + 한국어 커뮤니티 접근성 + IOCP/RIO 양쪽을 실패하며 쌓은 경험 |

### 2.3 SWOT

| | Helpful | Harmful |
|---|---------|---------|
| **Internal** | **Strengths**: 실측 벤치마크 3단계, 이미 해결한 수명/idle 이슈, Logger/TimerQueue 등 공용 유틸, C++20 모듈 정착 | **Weaknesses**: 1인 프로젝트(bus factor 1), 테스트 커버리지 부족(LibNetworksTests 뼈대), 문서 대부분 한국어만 |
| **External** | **Opportunities**: C++ IOCP 오픈 소스 레퍼런스 공백, 한국 인디 게임 서버 시장 재부상, MSVC 모듈 안정화 | **Threats**: RIO/io_uring 선호 추세, Photon/Unity 전용 솔루션 지배력, Windows 서버 시장 축소 |

**SO 전략**: 벤치마크 재현 가능성 + 한국어 아키텍처 문서로 공백을 선점 (블로그 시리즈 + README 리라이트).
**WT 전략**: v1 범위를 "IOCP 서버 엔진 핵심"으로 좁혀 RIO 분기를 일시 동결, 테스트 L1/L2 자산 확보로 bus factor 완화.

### 2.4 Strategic Analysis — 왜 IOCP 우선인가 (사용자 지시 해석)

- 사용자 지시: "우선 RIO 기능은 잠시 뒤로 미루고 IOCP 부터 최적화 완료한 게임 서버 엔진"
- 해석: **v1 = IOCP-only 게임 서버 엔진**. RIO 코드(`LibNetworksRIO`, `FastPortServerRIO`)는 현 상태로 "frozen / experimental" 태그 후 유지보수만.
- RIO 벤치마크·문서(`ARCHITECTURE_RIO.md`, `iocp-performance-benchmark-research.md`)는 v2 준비 자산으로 보존.
- v1 에서 RIO 관련 범위 변경 금지(ICE, 수명 이슈 등 발견 시만 방어적 수정).

---

## 3. Phase 3 — Research

### 3.1 User Personas (3)

#### Persona 1 — "인디 Jihoon" (Primary / Beachhead)

| 속성 | 값 |
|------|---|
| 역할 | 3인 인디팀의 서버 프로그래머 (C++ 8년, Unity 클라 팀원과 협업) |
| 프로젝트 | 소규모 MMORPG (목표 CCU 5K), Windows VPS 운영 |
| Pain | "Photon은 비싸고 블랙박스, 직접 IOCP 짜니 수명 레이스로 3일 날림" |
| JTBD | "내 게임에 붙일, 검증된 IOCP TCP 게임 서버 뼈대가 필요하다" |
| Trigger | 검색 "C++ IOCP 게임 서버 예제" / KGC 발표 / 블로그 포스트 |
| Success | 1주 내에 Echo → Chat 예제를 돌리고 자기 게임 프로토콜로 교체 |

#### Persona 2 — "시니어 Minseok" (Secondary)

| 속성 | 값 |
|------|---|
| 역할 | 중견 게임 회사 서버 팀 리드 (C++ 15년, 레거시 엔진 유지보수) |
| Pain | "사내 IOCP 엔진이 2010년대 레퍼런스 수준에서 멈춤. 신입 온보딩 자료 부족" |
| JTBD | "신입에게 보여줄 수 있는 '모던 C++20 IOCP 정석' 레퍼런스" |
| Trigger | 팀 내 리뷰 · 채용 교육 자료 탐색 |
| Success | 프로젝트를 교보재로 신입 3명이 IOCP 수명/지터 토픽을 설명 가능 |

#### Persona 3 — "학습자 Yuri" (Tertiary)

| 속성 | 값 |
|------|---|
| 역할 | C++ 3–5년차, 게임 서버 전향 희망 |
| Pain | "IOCP 자료는 많은데 '왜 그렇게 써야 하는지'를 설명한 최신 프로젝트가 없다" |
| JTBD | "C++20 모듈 기반 IOCP 서버를 코드로 이해하고 싶다" |
| Trigger | 블로그 시리즈, GitHub trending |
| Success | 벤치마크 재현 + 수명 이슈 Plan/Design/Analysis 문서 흐름 학습 |

### 3.2 Competitors (5)

| 경쟁자 | 카테고리 | 강점 | 약점 | FastPort vs |
|-------|----------|------|------|------------|
| **Photon Server / Quantum** | 상용 게임 서버 | 성숙, SDK 풍부, 매칭/룸 기능 | 유료, Windows/C# 중심, 블랙박스, 커스텀 어려움 | OSS · C++ · 커스터마이즈 자유 |
| **Mirror / FishNet** | Unity HLAPI | Unity 통합, 대규모 커뮤니티 | Unity 전용, TCP/KCP 수준, 저지연 게임 한계 | 엔진 독립, 초저지연 타깃 |
| **Agones** | 오케스트레이션 | K8s 기반 세션 관리 | 네트워크 라이브러리 아님 (레이어 다름) | 엔진 자체 + 운영 채널 |
| **boost.asio / Asio 직접 사용** | 범용 네트워크 | 크로스플랫폼, 검증 | 게임 도메인 기능 없음, 수명/지터 DIY | 게임 서버 컨벤션 내장 |
| **자체 사내 IOCP 엔진 (비공개)** | 대부분 중견 회사 | 프로젝트 적합 | 외부 공유 불가, 교육 자료 없음, 자주 신입이 재발명 | 공개 · 한국어 문서 · 재현 가능 벤치 |

### 3.3 TAM / SAM / SOM (이중 방법 추정)

**Top-down**:
- 전 세계 Windows 기반 C++ 게임 서버 개발 팀 (실시간/저지연) 약 8–12K 팀 추정 (Steam 멀티플레이어 릴리즈 수 기준).
- 한국 집중 대상: 인디/미드코어 C++ 서버 팀 ~500–800 팀.

**Bottom-up**:
- KGC/IGC 참석 서버 엔지니어 수 + C++ Korea 슬랙 활성 사용자 × 게임업계 비율 ≈ 300–600 명.

| 지표 | 수치 | 정의 |
|------|------|------|
| **TAM** | 전 세계 약 10K 팀 / 연 30K C++ Windows 게임 서버 엔지니어 | OSS 니즈 절대치 |
| **SAM** | 한국 + 일본 + 동아시아 C++ 게임 서버 팀 ~1.5K | 한국어/일본어 문서 타깃 |
| **SOM (2년)** | GitHub stars 500 / 월 방문 2K / 실제 도입 팀 10–20 | Beachhead 현실 목표 |

### 3.4 Customer Journey Map — Persona 1 (Jihoon)

| 단계 | 행동 | 감정 | 접점 | Opportunity |
|------|------|------|------|-------------|
| Aware | "C++ IOCP 게임 서버" 구글링 | 답답함 | 블로그, Stack Overflow | README 랜딩 강화, 한국어 블로그 |
| Consider | README의 벤치마크 표 확인 | 기대·의심 | docs/benchmark-results-*.md | 벤치마크 재현 스크립트 one-click |
| Try | `git clone` → vcpkg bootstrap → 빌드 | 막힘 위험 | BUILD_GUIDE.md | Windows 10/11 MSVC 최신 가이드 |
| Onboard | Echo 샘플 실행, FastPortBenchmark 실행 | "오 된다" | 샘플 · 벤치 리포트 | 3분 안에 결과 재현되는 샘플 |
| Integrate | 자기 .proto 로 교체, InboundSession 상속 | 수명/스레드 걱정 | ARCHITECTURE_IOCP.md | OnPacketReceived 수명 규약 문서 |
| Retain | 본인 게임 프로덕션 투입 | 안정성 요구 | Admin 채널, Log | ServerStatus · 카테고리 로그 |
| Advocate | 블로그 / 커뮤니티 공유 | 성취 | GitHub Discussions | Star / Issue / PR 유도 |

---

## 4. Phase 4 — Beachhead & GTM

### 4.1 ICP (Ideal Customer Profile)

- **핵심**: 한국 C++ 3년+ 개발자, Windows 서버 운영 경험, Unity/언리얼 클라이언트와 프로토콜 맞춤, CCU 1K–20K 실시간 게임(MMO/SLG/chat heavy).
- **Reject**: 모바일 턴제 · 순수 REST API 게임 · Linux 전용 스택 · 대형 MMO AAA (자체 엔진 보유).

### 4.2 Beachhead Scoring (Geoffrey Moore 4 criteria)

| Segment 후보 | Compelling Reason (1–5) | Access (1–5) | Reference Potential (1–5) | Adjacent Market (1–5) | Total |
|--------------|:-:|:-:|:-:|:-:|:-:|
| **한국 C++ 인디 서버팀** | 5 | 5 | 4 | 4 | **18** ✅ |
| 한국 중견 서버 팀 리드 | 3 | 3 | 5 | 4 | 15 |
| 글로벌 OSS C++ 네트워크 사용자 | 3 | 2 | 3 | 3 | 11 |
| C++ 학습자 | 4 | 4 | 2 | 3 | 13 |

**Selected Beachhead**: 한국 C++ 인디 서버팀.

### 4.3 GTM Strategy

| 축 | 상세 |
|---|------|
| **Positioning** | "한국어로 읽는 모던 C++20 IOCP 게임 서버 레퍼런스 엔진" |
| **Channels** | GitHub README (한/영) · 블로그 시리즈(5편, 한국어) · KGC/C++ Korea 발표 · YouTube 벤치마크 재현 영상 |
| **Conversion Funnel** | 블로그 읽기 → README → `git clone` → 벤치마크 재현 → 샘플 커스터마이즈 |
| **Pricing** | MIT 라이선스, OSS 무료 |
| **Metrics** | Stars 500 (2년), clone/week 50, 벤치마크 재인용 5건, 실도입 팀 10+ |
| **Battlecards** | ① Photon 대비: 커스텀·OSS 강조 ② asio 대비: 게임 도메인 내장 ③ Mirror 대비: 엔진 독립 |

### 4.4 Growth Loops

```
Content Loop:
블로그 포스트(이슈별: lifetime race, zero-copy) →
  한국 커뮤니티 공유 →
    GitHub Star/Issue →
      실전 피드백 →
        다음 블로그 소재
```

---

## 5. Phase 5 — PRD 8-Section

### 5.1 Overview
FastPort IOCP Game Server Engine v1 은 Windows IOCP 기반의 TCP 게임 서버 엔진으로, 수신/송신 Zero-Copy 경로, 세션 수명 안전성, idle 감지, Admin 채널, 한국어 아키텍처 문서를 포함한 **"IOCP 레퍼런스 완성판"** 을 목표로 한다.

### 5.2 Goals & Non-Goals

**Goals**
- G1: **수신 경로** Zero-Copy 완성 (PacketFramer span 반환, OnPacketReceived span 전달, 오브젝트 풀)
- G2: **IOSession lifetime race** 완전 해결 (기존 Plan/Design Option C Pragmatic 구현 + Stress 1M pass)
- G3: **성능 목표 확정**: P50 ≤ 30µs, P99(stable) ≤ 80µs, Peak ≥ 30K PPS(64B)
- G4: **공격적 Keep-Alive 튜닝 API** 노출 (idle=5s, interval=1s, probes=3 기본)
- G5: **Graceful Shutdown** 파이프라인 (리스너 stop → in-flight drain → 세션 close → worker join)
- G6: **문서 & 샘플**: 한국어 ARCHITECTURE_IOCP.md v2, Echo/Chat 샘플, 벤치마크 재현 스크립트
- G7: **LibNetworksTests L1 커버리지 70%+** (핵심 클래스)

**Non-Goals (v1)**
- RIO 관련 개선 (코드는 freeze, 치명 버그만 방어적 수정)
- UDP / QUIC 지원
- 분산 멀티노드 (매칭/룸 디스트리뷰션)
- 크로스 플랫폼 (Linux/macOS)
- 암호화 (TLS) — v2로
- 쓰레드 풀 튜닝 자동화

### 5.3 Requirements (Functional / Non-Functional)

**Functional**
- FR-1: `PacketFramer::TryFrameFromBuffer` 가 `std::span<const std::byte>` 를 반환 (복사 제거).
- FR-2: `IOSession::OnPacketReceived(PacketHeader, std::span<const std::byte> payload)` — 콜백 생애는 동기 범위로 한정, 비동기 사용 시 복사 책임은 사용자.
- FR-3: `Packet` / `OverlappedEx` 오브젝트 풀 (per-IOService).
- FR-4: `Socket::SetKeepAlive(idleSec, intervalSec, probes)` API.
- FR-5: `IOService::Shutdown(graceful=true)` — drain + worker join.
- FR-6: IOSession lifetime race 수정 (기존 `docs/01-plan/.../iosession-lifetime-race.plan.md` Option C 따름).
- FR-7: Admin 채널에서 per-session RX/TX/last-activity 노출.

**Non-Functional**
- NFR-1: Release x64 기준, 로컬 loopback 64B echo: Peak ≥ 30K PPS, P50 ≤ 30µs, P99(stable) ≤ 80µs.
- NFR-2: FastPortBenchmark 로 재현 가능한 환경 정보(`docs/benchmark-results-04-*.md`) 기록 의무화.
- NFR-3: Stress reproducer(IOSession lifetime) 1,000,000 회 × 2 run = 0 crash / 0 asan(MSVC /RTC).
- NFR-4: `/std:c++20` 고정, `stdcpplatest` 승격 금지 (C1001 ICE 이력).
- NFR-5: 모든 로그는 `LibCommons::Logger`, spdlog 직접 호출 금지, Shutdown 순서 준수(TimerQueue → Logger).

### 5.4 Success Criteria / KPI

| # | 지표 | 목표 |
|---|------|------|
| SC-1 | FastPortBenchmark Peak PPS | ≥ 30,000 |
| SC-2 | Stable P99 | ≤ 80µs |
| SC-3 | Stress reproducer (lifetime race) | 0 crash / 1M iter × 2 run |
| SC-4 | LibNetworksTests L1 통과율 | ≥ 95%, 커버리지 ≥ 70% |
| SC-5 | Echo 샘플 first-run 시간 | ≤ 10분 (clone→run) |
| SC-6 | GitHub Stars (6개월) | ≥ 100 (Beachhead 트랙) |
| SC-7 | 한국어 블로그 시리즈 | 5편 배포 |

### 5.5 User Stories → Section 6.2 참조

### 5.6 Dependencies

- **Blocking (이미 완료)**: Scatter-Gather Send, Zero-Copy Send, Session Idle Timeout, ServerStatus Admin, RIO 분리.
- **In-progress**: IOSession lifetime race (Plan/Design 완료, 구현 중)
- **Needed new**: Object Pool, Zero-Copy Recv, Graceful Shutdown, Keep-Alive API, Benchmark 재현 스크립트, 샘플 Chat.

### 5.7 Risks → Pre-mortem (Section 6.1)

### 5.8 Timeline / Milestones

| Milestone | 기간 (제안) | 산출 |
|-----------|-------------|------|
| M1: Lifetime race patch + stress green | +2주 | IOSession race 수정 반영 |
| M2: Zero-Copy Recv + Object Pool | +3주 | PacketFramer span, Packet Pool |
| M3: Keep-Alive API + Graceful Shutdown | +2주 | FR-4, FR-5 |
| M4: Benchmark 재현 스크립트 + 결과 md | +1주 | `benchmark-results-04-zero-copy-recv.md` |
| M5: 샘플(Echo, Chat) + 문서 v2 | +2주 | 한국어 가이드, README 리라이트 |
| M6: 공식 v1.0.0 릴리스 | +1주 | Git tag, 블로그 1편 공개 |

---

## 6. Phase 6 — Pre-mortem · Stories · Tests

### 6.1 Pre-mortem (Top 3 Risks)

| # | 상상한 실패 시나리오 | 조기 경보 | 완화책 |
|---|---------------------|----------|--------|
| R1 | Zero-Copy Recv 도입 중 수명 규약을 오해한 사용자가 span dangling 오류 발생 | 샘플에서도 재현, 이슈 리포트 | API 문서에 **동기 범위 전용** 명시 + 비동기 사용 시 복사 헬퍼(`ToVector(span)`) 제공 + 정적 체커 예제 |
| R2 | lifetime race 수정이 Option C 로 부분적으로만 막혀서 edge case(디스커넥트 + 재연결 경합)에서 재발 | Stress reproducer 확장 실패 | Stress를 "connect flood + disconnect flood" 시나리오로 확장, L2 runtime 테스트 추가 |
| R3 | 성능 목표(30K PPS, P99 80µs)가 특정 머신에만 재현되어 "거짓 광고" 논란 | README 이슈 제기 | 벤치마크 md 에 CPU/NIC/Windows build 기록 의무, 결과 range 로 표기, 재현 스크립트 제공 |

### 6.2 User Stories (INVEST)

| ID | Story | Acceptance | INVEST |
|----|-------|------------|--------|
| US-1 | As a 게임 서버 개발자, OnPacketReceived 에서 span 을 받아 복사 없이 파싱하고 싶다 | span 전달 + docs 수명 명시 + 복사 헬퍼 존재 | ✅ |
| US-2 | As a 개발자, Packet 객체 생성 지터를 없애고 싶다 | Object Pool 사용, 1M send/recv 시 할당 0 (ETW 계측) | ✅ |
| US-3 | As a 개발자, 클라이언트 비정상 단절을 5초 내에 감지하고 싶다 | SetKeepAlive(5,1,3) + idle timeout 조합 동작 확인 | ✅ |
| US-4 | As a 운영자, graceful shutdown 시 in-flight I/O 가 새지 않도록 하고 싶다 | `Shutdown(graceful)` 이후 pending WSARecv/Send 0 확인 | ✅ |
| US-5 | As a 신규 사용자, 10분 내 Echo 샘플을 돌리고 싶다 | clone→build→run→벤치 결과 출력 가이드 | ✅ |
| US-6 | As a 운영자, Admin 채널로 세션별 RX/TX 를 조회하고 싶다 | ServerStatus v2 응답에 필드 포함 | ✅ |
| US-7 | As a 리드 개발자, 이 엔진이 수명 이슈 없이 1M 스트레스를 통과하는지 확인하고 싶다 | CI 에 stress reproducer run 포함 | ✅ |

### 6.3 Test Scenarios

**L1 — Unit**
- PacketFramer Zero-Copy: 완전/partial/invalid/split 페이로드 케이스.
- Object Pool: concurrent 1024 thread × 1000 alloc/free = 할당 수 ≤ pool size.
- KeepAlive setter: Windows `WSAIoctl SIO_KEEPALIVE_VALS` 호출 검증.

**L2 — Integration**
- Echo 서버 (127.0.0.1:port) × 1 client × 10K messages → Peak/P50/P99 기록.
- Chat 샘플 × 100 client × 5분 → 메모리 leak 없음 (PrivateBytes ±10%).
- Graceful shutdown: 1000 세션 접속 중 Shutdown(graceful) → 모든 세션 close 이벤트 + worker join.

**L3 — Stress / Reliability**
- IOSession lifetime race: connect-burst(1K) + disconnect-burst(1K) 1M iter = 0 crash.
- Keep-Alive: 클라이언트 강제 케이블 단절 시뮬(Windows Filter) → ≤ 10초 disconnect.

**L4 — Perf (Enterprise)**
- 워크스테이션 A(Zen4) / 워크스테이션 B(Intel 13세대) 에서 FastPortBenchmark 각 10 run → 결과 md.

---

## 7. Phase 7 — Stakeholder Map · Attribution

### 7.1 Stakeholder Map

| Stakeholder | 관심사 | 참여 방식 |
|-------------|--------|-----------|
| 본인(오너) | 엔진 품질, 학습, 포트폴리오 | Owner |
| 한국 C++ 인디 서버 팀 | 실전 도입, 성능, 문서 | Primary user |
| C++ Korea · KGC 커뮤니티 | 교육·전파 | Advocate |
| 상용 경쟁(Photon) | 대안 포지셔닝 | Competitor (간접) |
| MSVC 팀 (C1001 ICE 사례) | 툴체인 이슈 피드백 | Upstream (간접) |

### 7.2 Attribution

PM Agent Team 프레임워크는 [pm-skills](https://github.com/phuryn/pm-skills) (Pawel Huryn, MIT License) 에서 차용한 구조를 한국어 및 IOCP 게임 서버 도메인에 맞게 조합했다. Opportunity Solution Tree 는 Teresa Torres (*Continuous Discovery Habits*), Beachhead Scoring 은 Geoffrey Moore (*Crossing the Chasm*), JTBD 6-Part VP 는 Tony Ulwick/strategyn 계열을 참고했다.

### 7.3 PDCA Handoff

- **다음 단계**: `/pdca plan iocp-game-server-engine` — 이 PRD 가 Plan 문서의 컨텍스트로 자동 참조됨.
- **권장**: IOSession lifetime race 수정(`iosession-lifetime-race`)이 선행 병렬로 진행 중이므로, v1 Plan 은 그 결과를 merge 한 시점 기준으로 M2(Zero-Copy Recv + Object Pool) 부터 범위화.

