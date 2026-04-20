# iosession-lifetime-race Design Document

> **Summary**: IOCP/RIO worker 의 use-after-free 를 Outstanding-I/O self-retain 으로 해결. Option C (Pragmatic) — posting 직전 `SelfRetain = shared_from_this()`, completion 진입 즉시 `auto self = std::move(...)` 한 줄로 lifecycle 보장.
>
> **Project**: FastPort
> **Author**: AnYounggun
> **Date**: 2026-04-21
> **Status**: Draft
> **Planning Doc**: [iosession-lifetime-race.plan.md](../../01-plan/features/iosession-lifetime-race.plan.md)

---

## Context Anchor

| Key | Value |
|-----|-------|
| **WHY** | 3000 conn stress 에서 confirmed use-after-free 크래시 (freed heap fill 증거). 런타임 안정성 직결 — 프로덕션/장기 배포 블로커. |
| **WHO** | FastPort 엔진 유지보수자. 간접적으로 모든 FastPort 기반 게임/서비스. |
| **RISK** | 세션 lifetime 구조 변경이 기존 `RequestDisconnect` / `OnDisconnected` 흐름과 충돌, 순환 참조로 세션 영원히 소멸 안 됨, shared_ptr ref-count 경합으로 인한 성능 저하. |
| **SUCCESS** | 10k conn × 5분 stress 중 UAF 크래시 0회 (Debug·Release 양쪽), 64개 기존 L1 회귀 0, 기능 외부 동작 무변화, 세션 destruct 정상 발생 (leak 0). |
| **SCOPE** | IOCP `IOSession` + RIO `RIOSession` 양쪽 동시 수정. `IIOConsumer` 인터페이스 불변. 10k conn stress reproducer 도구 포함. |

---

## 1. Overview

### 1.1 Design Goals

- IOCP/RIO worker 가 raw `this` pointer 로 dispatch 하는 기존 구조를 유지하면서도, I/O 가 pending 인 동안 세션 객체가 **절대로 소멸되지 않도록** 보장한다.
- 새 클래스·인터페이스 추가 없이 **표준 `shared_ptr` + move semantics** 로 lifecycle invariant 를 표현한다.
- IOCP 와 RIO 양쪽에 **동일 패턴**을 적용해 대칭 유지, 미래 확장 시 복제 용이.
- 성능 오버헤드 최소화 — atomic ref-count 조작은 I/O posting 당 1~2회 (실무상 3000 conn × 1Hz ≈ 6k ops/s, 무시 가능).

### 1.2 Design Principles

1. **Lifetime invariant 를 코드 한 줄로 표현** — `auto self = std::move(m_RecvOverlapped.SelfRetain);` 만 봐도 "이 함수 종료까지 세션 alive" 를 이해 가능.
2. **Exit-path 추적 금지** — early return 이 6~10개 있어도, `std::shared_ptr` destructor 가 자동 drop.
3. **Posting 실패만 inline reset** — 완료 통지 안 올 것이므로 보류하면 안 됨 (3곳).
4. **Invariant breakage 조기 발견** — session destruct 로그로 outstanding I/O count 관찰.
5. **Production 코드 스타일 일관성** — 기존 FastPort 네이밍·모듈 규약·`LibCommons::Logger` 그대로 사용.

---

## 2. Architecture Options (v1.7.0)

### 2.0 Architecture Comparison

| Criteria | Option A: Minimal | Option B: Clean | **Option C: Pragmatic** |
|---|:-:|:-:|:-:|
| Approach | Inline retain/reset 각 exit 명시 | RAII `ScopedIoRetain` 헬퍼 | move-out 한 줄로 lifecycle 자동 |
| New Files | 0 | 1 (헬퍼) | 0 |
| Modified Files | 4 (IOSession/RIOSession × 2) | 5 (헬퍼 + 위 4) | **4** |
| Complexity | Low | High | **Medium** |
| Maintainability | Medium (reset 누락 리스크) | High (RAII) | **High (move semantics)** |
| Effort | Medium (exit path 매핑) | High | **Low** |
| Risk | High (누락) | Low (과설계 우려) | **Minimal** |

**Selected**: **Option C — Pragmatic** (Checkpoint 3: user 선택)

**Rationale**: Recv/Send 분기의 exit path 가 총 10개 전후 — A 는 누락 리스크, B 는 새 클래스 도입이 FastPort 스타일(C++20 + 명시 흐름) 과 괴리. C 는 표준 `std::shared_ptr` + `std::move` 로 lifecycle 자동화, 추가 추상 0개, 코드 변경 최소.

---

### 2.1 Component Diagram

```
┌──────────────────────────────────────────────────────────────────┐
│                          IOCP 경로                                │
│                                                                  │
│   IOService worker                                               │
│   ──────────────                                                 │
│   GetQueuedCompletionStatus → (key = raw IOSession*)             │
│       ↓                                                          │
│   IIOConsumer::OnIOCompleted  ◀── 인터페이스 불변                │
│       ↓                                                          │
│   IOSession::OnIOCompleted                                       │
│     ├─ Recv 분기:  auto self = move(m_RecvOverlapped.SelfRetain) │
│     │    └ (기존 로직 그대로, 모든 early return 에서 self 자동 drop)│
│     └─ Send 분기:  auto self = move(m_SendOverlapped.SelfRetain) │
│          └ (동일)                                                │
│                                                                  │
│   IOSession::RequestRecv / TryPostSendFromQueue                  │
│     ├─ m_*Overlapped.SelfRetain = shared_from_this();            │
│     ├─ ::WSARecv / ::WSASend(...)                                │
│     └─ 실패 시 inline reset(), pending/success 는 retain 유지    │
├──────────────────────────────────────────────────────────────────┤
│                          RIO 경로                                │
│                                                                  │
│   RIOService completion loop                                     │
│   ─────────────────────                                          │
│   (RIO_CQ dequeue) → requestContext (raw RIOSession*)            │
│       ↓                                                          │
│   RIOSession::OnRioIOCompleted                                   │
│     ├─ Send 완료: auto self = move(m_SendContext.SelfRetain)     │
│     └─ Recv 완료: auto self = move(m_RecvContext.SelfRetain)     │
│                                                                  │
│   RIOSession::RequestRecv / Send posting                         │
│     ├─ m_*Context.SelfRetain = shared_from_this();               │
│     ├─ RIOReceive / RIOSend                                      │
│     └─ 실패 시 inline reset                                      │
└──────────────────────────────────────────────────────────────────┘
```

### 2.2 Data Flow

```
  accept / connect
      ↓
  session = make_shared<IOSession>
  sessions.Add(id, session)        ← shared_ptr 1개 (container)
      ↓
  RequestReceived()
   └ RequestRecv(true)
       └ SelfRetain = shared_from_this()   ← shared_ptr 2개 (container + SelfRetain)
         WSARecv posted

  … 시간 경과 …

  [다른 스레드] RequestDisconnect
   └ Shutdown+Close → 기존 pending WSARecv 실패 완료 예약
   └ OnDisconnected → sessions.Remove(id)   ← container 제거, shared_ptr 1개로 감소
                                              (SelfRetain 이 아직 쥐고 있으므로 alive)

  [IOCP worker] GetQueuedCompletionStatus 로 pending 실패 완료 dequeue
      ↓
  IOSession::OnIOCompleted(false, …, &m_RecvOverlapped.Overlapped)
      ↓
  auto self = move(m_RecvOverlapped.SelfRetain);   ← shared_ptr 이동
    (bSuccess=false → RequestDisconnect(이미 CAS true 므로 no-op), return)
      ↓
  함수 종료 → self drop → shared_ptr 0 개 → ~IOSession() 안전 호출
```

### 2.3 Dependencies

| Component | Depends On | Purpose |
|---|---|---|
| `IOSession` | `std::enable_shared_from_this<IOSession>` | shared_from_this() 로 retain 생성 |
| `IOSession::OverlappedEx` | `std::shared_ptr<IOSession>` | Recv/Send 각각 SelfRetain 보유 |
| `RIOSession` | `std::enable_shared_from_this<RIOSession>` | 동일 |
| `RIOSession` Send/Recv Context | `std::shared_ptr<RIOSession>` | 동일 |
| `IOService` / `RIOService` | (변경 없음) | 기존 completion 경로 유지 |
| `IIOConsumer` | (변경 없음) | 인터페이스 불변 — Plan Q7=a |

---

## 3. Data Model

### 3.1 IOSession `OverlappedEx` 확장

```cpp
// IOSession.ixx — protected nested struct (server-status 에서 이미 private → protected 로 승격)
struct OverlappedEx
{
    OVERLAPPED            Overlapped{};
    std::vector<char>     Buffers{};
    std::vector<WSABUF>   WSABufs{};
    size_t                RequestedBytes = 0;
    bool                  IsZeroByte = false;

    // [NEW] Outstanding I/O self-retain.
    // posting 직전 shared_from_this() 로 설정 → completion 진입 시 move-out.
    // Lifecycle invariant: 값이 non-null 인 동안은 대응 WSARecv/WSASend 가 pending.
    std::shared_ptr<IOSession> SelfRetain;

    void ResetOverlapped()
    {
        std::memset(&Overlapped, 0, sizeof(Overlapped));
    }
};
```

### 3.2 RIOSession Send/Recv Context 확장

```cpp
// RIOSession.ixx — 기존 context 구조체에 shared_ptr 추가
struct RioRequestContext
{
    // ... 기존 fields (RIO_BUFFERID, offset, length 등) ...

    // [NEW] RIO 쪽도 동일 패턴. IOCP 의 SelfRetain 과 대칭.
    std::shared_ptr<RIOSession> SelfRetain;
};
```

**주의**: RIO 의 `requestContext` (ULONG_PTR) 에는 **raw `this` pointer 를 그대로 유지** (Plan Q8=a). SelfRetain 은 session 멤버로만 존재하여 RIO completion key 규약에 영향을 주지 않는다.

### 3.3 Invariants

| Invariant | 보장 방법 |
|---|---|
| I1: `SelfRetain` 은 한 번에 **대응 I/O 1개** 에만 연결 | `m_RecvInProgress` / `m_SendInProgress` CAS 로 이미 동시 posting 방지 |
| I2: posting 이후 반드시 완료 통지 1개 도착 | Windows IOCP/RIO 의 완료 통지 보장 (Shutdown+Close 시 실패 통지) |
| I3: completion 진입 즉시 `move(SelfRetain)` | OnIOCompleted Recv/Send 분기의 첫 줄 (pointer 매칭 이후) |
| I4: posting 실패 시 inline reset | 3개 site: IOCP Recv, IOCP Send, RIO posting 경로 |
| I5: 세션 소멸은 모든 SelfRetain reset 이후에만 | shared_ptr ref-count 로 자동 보장 |

---

## 4. API Specification

> **Note**: 이 feature 는 외부 API / 네트워크 프로토콜 변경 없음. "API" 는 internal C++ 함수 계약.

### 4.1 Posting Pattern (통합 규약)

```cpp
// IOCP RequestRecv — 모든 경로 (zero-byte, real)
bool IOSession::RequestRecv(bool bZeroByte)
{
    m_RecvOverlapped.ResetOverlapped();
    m_RecvOverlapped.IsZeroByte = bZeroByte;
    m_RecvOverlapped.WSABufs.clear();

    // ... WSABufs 구성 (기존 로직) ...

    // [NEW] posting 직전 retain. 실패 시 reset.
    m_RecvOverlapped.SelfRetain = shared_from_this();

    DWORD flags = 0, bytes = 0;
    int result = ::WSARecv(m_pSocket->GetSocket(),
        m_RecvOverlapped.WSABufs.data(),
        static_cast<DWORD>(m_RecvOverlapped.WSABufs.size()),
        &bytes, &flags,
        &m_RecvOverlapped.Overlapped,
        nullptr);

    if (result == SOCKET_ERROR)
    {
        int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            // [NEW] 완료 통지 안 올 것 → inline reset
            m_RecvOverlapped.SelfRetain.reset();
            LibCommons::Logger::GetInstance().LogError("IOSession",
                "RequestRecv() WSARecv failed. Session Id : {}, Error Code : {}, ZeroByte : {}",
                GetSessionId(), err, bZeroByte);
            return false;
        }
    }

    return true;
}
```

```cpp
// IOCP TryPostSendFromQueue — 동일 패턴
bool IOSession::TryPostSendFromQueue()
{
    bool expected = false;
    if (!m_SendInProgress.compare_exchange_strong(expected, true)) return true;

    // ... buffer 구성 (기존 로직) ...

    if (bytesToSend == 0) { m_SendInProgress.store(false); return true; }

    // [NEW] posting 직전 retain
    m_SendOverlapped.SelfRetain = shared_from_this();

    int result = ::WSASend(..., &m_SendOverlapped.Overlapped, nullptr);
    if (result == SOCKET_ERROR)
    {
        int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            // [NEW] inline reset
            m_SendOverlapped.SelfRetain.reset();
            m_SendInProgress.store(false);
            return false;
        }
    }
    return true;
}
```

### 4.2 Completion Pattern (OnIOCompleted)

```cpp
void IOSession::OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped)
{
    if (!pOverlapped) return;

    if (pOverlapped == &(m_RecvOverlapped.Overlapped))
    {
        // [NEW] 진입 즉시 ownership 이동. 함수의 어떤 exit 에서든 자동 drop.
        auto self = std::move(m_RecvOverlapped.SelfRetain);
        (void)self;   // 실제 사용은 destructor 에서 암묵적

        // ----- 이하 기존 Recv 로직 100% 그대로 -----
        if (!bSuccess) {
            m_RecvInProgress.store(false);
            LibCommons::Logger::GetInstance().LogInfo("IOSession",
                "OnIOCompleted() Recv failed. Session Id : {}, Error Code : {}",
                GetSessionId(), GetLastError());
            RequestDisconnect();
            return;
        }

        if (m_RecvOverlapped.IsZeroByte) {
            if (bytesTransferred == 0) {
                if (!RequestRecv(false)) {
                    m_RecvInProgress.store(false);
                    RequestDisconnect();
                }
                return;
            } else {
                m_RecvInProgress.store(false);
                return;
            }
        } else {
            // Real Recv 경로 (server-status 에서 추가된 bytes 카운터 포함)
            // ... CommitWrite, fetch_add, ReadReceivedBuffers, RequestReceived ...
            return;
        }
        // [어떤 return 이 실행되든 self 는 여기서 drop]
    }

    if (pOverlapped == &(m_SendOverlapped.Overlapped))
    {
        // [NEW] Send 도 동일
        auto self = std::move(m_SendOverlapped.SelfRetain);
        (void)self;

        // ----- 이하 기존 Send 로직 그대로 -----
        m_SendInProgress.store(false);
        if (!bSuccess) { RequestDisconnect(); return; }
        // ... Consume, fetch_add, OnSent, hasPending 재posting ...
        return;
    }
}
```

### 4.3 RIO 대칭 패턴

```cpp
// RIOSession posting — IOCP 와 동일 구조
void RIOSession::RequestRecv()
{
    m_RecvContext.SelfRetain = shared_from_this();
    bool ok = RioExtension::GetInstance().RIOReceive(
        m_RequestQueue, &m_RecvBuffer, 1, 0,
        reinterpret_cast<PVOID>(&m_RecvContext));
    if (!ok) {
        m_RecvContext.SelfRetain.reset();
        // ... error log, RequestDisconnect ...
    }
}

void RIOSession::OnRioIOCompleted(RIO_RESULT const& result)
{
    auto* pCtx = reinterpret_cast<RioRequestContext*>(result.RequestContext);
    auto self = std::move(pCtx->SelfRetain);
    (void)self;

    // ... 기존 Send/Recv 분기 로직 그대로 ...
}
```

### 4.4 세션 소멸 시 로그 (FR-11)

```cpp
IOSession::~IOSession()
{
    // leak 조기 탐지: destructor 진입 시점에 SelfRetain 이 non-null 이면 invariant 위반.
    if (m_RecvOverlapped.SelfRetain || m_SendOverlapped.SelfRetain)
    {
        LibCommons::Logger::GetInstance().LogError("IOSession",
            "~IOSession() SelfRetain still set! Session Id : {}, Recv : {}, Send : {}",
            GetSessionId(),
            static_cast<bool>(m_RecvOverlapped.SelfRetain),
            static_cast<bool>(m_SendOverlapped.SelfRetain));
    }
    else
    {
        LibCommons::Logger::GetInstance().LogDebug("IOSession",
            "~IOSession() Session Id : {}", GetSessionId());
    }
}
```

실제로 `SelfRetain` 이 non-null 이면 우리 자신이 destructor 안에 있을 수 없음 (순환 참조). 위 분기는 **논리적으로 절대 trigger 되지 않아야 하는** paranoid check — 로직 자체의 invariant 증명.

---

## 5. UI/UX Design

### 5.1 FastPortTestClient Stress Mode 확장

현재 Scale 테스트는 단순 대량 연결만 수행. Stress reproducer 는 **연결 + 반복 disconnect/reconnect** 를 5분간 실행.

```
┌─────────────────────────────────────────────────────┐
│ FastPortTestClient                                  │
│ ┌─────────────────────────────────────────────────┐ │
│ │ [Connection] [Echo] [Benchmark] [Metrics]       │ │
│ │ [A/B] [Admin] [Stress] ◀ NEW                    │ │
│ └─────────────────────────────────────────────────┘ │
│                                                     │
│  Stress Mode                                        │
│  ─────────────────────────────────────────────────  │
│  Target IP/Port:   [127.0.0.1]:[7777]               │
│  Target Conns:     [10000]                          │
│  Churn rate:       [100] /sec (disconnect+reconnect)│
│  Duration:         [300] sec                        │
│  Server mode:      ● IOCP  ○ RIO                    │
│                                                     │
│  [Start Stress]  [Stop]                             │
│                                                     │
│  Stats (live):                                      │
│    Total Accepted : 30123                           │
│    Active         : 9987                            │
│    Churned        : 20136                           │
│    Elapsed        : 201 / 300 sec                   │
│    Crash detected : None   ← 빨간색으로 변함        │
│                                                     │
│  Log tail (last 5):                                 │
│    [INFO] session destructed id=123 ok              │
│    ...                                              │
└─────────────────────────────────────────────────────┘
```

### 5.2 Stress Mode User Flow

```
사용자
  ↓
FastPortServer(.exe) 별도 프로세스 실행 (IOCP 또는 RIO)
  ↓
TestClient Stress 탭 → 파라미터 설정 → [Start Stress]
  ↓
TestClient 내부:
  1. Target Conns 개수만큼 IOService 에 연결 posting
  2. 타이머 기반 churn loop: 매 10ms 마다 churn_rate/100 개 disconnect + reconnect
  3. 실시간 stats 표시
  ↓
5분 경과 또는 [Stop] → 모든 연결 종료 + 최종 stats 출력
  ↓
사용자: FastPortServer 로그 검사 (destruct = accept 일치, 크래시 없음)
```

### 5.3 Page UI Checklist

#### Stress Tab

- [ ] Input: Target Conns (numeric, 1~50000, default 10000)
- [ ] Input: Churn rate /sec (numeric, 0~1000, default 100)
- [ ] Input: Duration seconds (numeric, 10~3600, default 300)
- [ ] Radio: Server mode (IOCP / RIO) — **UI only**, 서버는 별도 프로세스
- [ ] Button: Start Stress (disabled while running)
- [ ] Button: Stop (enabled while running)
- [ ] Stat: Total Accepted (cumulative connect 성공 카운터)
- [ ] Stat: Active (현재 살아있는 연결 수)
- [ ] Stat: Churned (disconnect + reconnect 성공 사이클 수)
- [ ] Stat: Elapsed / Target duration
- [ ] Stat: Crash detected (boolean, 연결 실패 스파이크 감지 시 활성)
- [ ] Log tail: 최근 5개 TestClient 측 로그 출력

---

## 6. Error Handling

### 6.1 Error Scenarios

| # | Scenario | 발생 경로 | 처리 방식 |
|---|---|---|---|
| E1 | WSARecv immediate failure (e.g. WSAENOTSOCK) | `RequestRecv` posting | inline `SelfRetain.reset()` + log + return false |
| E2 | WSASend immediate failure | `TryPostSendFromQueue` | inline reset + `m_SendInProgress = false` + return false |
| E3 | `shared_from_this()` 호출 시점에 외부 shared_ptr 이 없음 | Accept 직후 container 등록 전 posting 발생 시 | 발생 불가 — `OnAccepted` (`InboundSession::OnAccepted`) 에서 container 등록 후 `RequestReceived` 호출. 만약 재구성 시 이 invariant 가 깨지면 `std::bad_weak_ptr` throw → 명확한 crash 로 로직 오류 조기 발견 |
| E4 | Socket close 후 pending 완료 도착 | 정상 흐름 | `OnIOCompleted(bSuccess=false)` → `SelfRetain` move-out → drop → 안전 소멸 |
| E5 | Destructor 에 SelfRetain 남아있음 | 순환 참조 or invariant 버그 | `~IOSession()` 에서 `LogError` — 논리상 발생 불가 |
| E6 | 두 번 reset (posting 실패 + completion 도착) | posting 실패 시 완료 안 옴 | move 는 이미 null 인 shared_ptr 에 대해 no-op — 안전 |
| E7 | Destruct 안 되는 세션 (container remove 했는데도 alive) | outstanding completion 대기 중 | 정상. 완료 도착 시 move-out → destruct. 정지되면 heap leak 로 관찰 |

### 6.2 Logging Contract

| Category | Level | When |
|---|---|---|
| `"IOSession"` | INFO | 정상 disconnect, session destruct (ok) |
| `"IOSession"` | ERROR | WSARecv/WSASend immediate failure, `~IOSession` 에 SelfRetain 남아있음 (invariant 위반) |
| `"RIOSession"` | INFO/ERROR | 동일 |
| `"Stress"` (신규) | INFO | 매 10초 요약 (active, churned, elapsed) |
| `"Stress"` | ERROR | 연결 실패 스파이크 감지 — crash 의심 |

---

## 7. Security Considerations

본 피처는 **내부 lifetime 관리**만 수정하며 외부 공격면 변경 없음. 기존 보안 특성 유지.

- [x] 입력 검증: 변경 없음 (패킷 파싱 경로 수정 없음)
- [x] 인증/인가: 변경 없음
- [x] 민감 정보 암호화: 변경 없음
- [x] 네트워크: 변경 없음
- [x] Rate limiting: 변경 없음

추가 고려: **Stress reproducer 가 외부 공격 벡터가 되지 않도록** — FastPortTestClient 내에서만 실행되며, Target IP 는 사용자 입력 (기본 127.0.0.1). 악의적 외부 서버로 DoS 도구로 쓰이지 않도록 UI 에 "Local loopback only" 경고 표시.

---

## 8. Test Plan (v2.3.0)

### 8.1 Test Scope

| Type | Target | Tool | Phase |
|------|--------|------|-------|
| L1 Unit | IOSession SelfRetain lifecycle (posting / completion / failure) | LibNetworksTests | Do |
| L1 Unit | RIOSession SelfRetain lifecycle | LibNetworksTests | Do |
| L1 Unit | Destructor paranoid check 동작 | LibNetworksTests | Do |
| L3 Stress | 10k conn × 5min reconnect, UAF 0회 + leak 0 | FastPortTestClient Stress 탭 | Do |
| Regression | LibNetworksTests 64 개 기존 테스트 | vstest.console | Check |

### 8.2 L1 — IOSession Lifetime Tests (`IOSessionLifetimeTests.cpp`)

| # | Test | Assertion |
|---|------|-----------|
| LT-01 | `SelfRetain_InitiallyNull` | 생성 직후 `m_RecvOverlapped.SelfRetain == nullptr` (inspector 활용) |
| LT-02 | `PostingSuccess_RetainHeld` | `RequestRecv(true)` 호출 → external shared_ptr count 증가 확인 (`use_count() >= 2`) |
| LT-03 | `CompletionMovesOut_ThenDrops` | 시뮬된 OnIOCompleted 후 SelfRetain == nullptr, use_count 복귀 |
| LT-04 | `PostingFailure_ResetsInline` | 소켓 INVALID_SOCKET 상태에서 `RequestRecv` → false 반환, SelfRetain 즉시 null |
| LT-05 | `DestructReleasesWhenNoRefs` | container 에 안 넣고 external ref 해제 → destructor 호출 확인 (weak_ptr.expired()) |
| LT-06 | `SendRetainSymmetry` | `TryPostSendFromQueue` 도 동일 패턴으로 동작 |

### 8.3 L1 — RIOSession Lifetime Tests (`RIOSessionLifetimeTests.cpp`)

| # | Test | Assertion |
|---|------|-----------|
| RT-01 | `RIO_PostingSuccess_RetainHeld` | RIO Receive posting 후 use_count 증가 |
| RT-02 | `RIO_CompletionMovesOut` | 시뮬된 completion 후 SelfRetain null |
| RT-03 | `RIO_PostingFailure_ResetsInline` | 잘못된 RQ 상태에서 RIOReceive 실패 → reset |
| RT-04 | `RIO_SendRetainSymmetry` | Send 도 동일 |

### 8.4 L1 — Destructor Paranoid Check

| # | Test | Assertion |
|---|------|-----------|
| DT-01 | `Destructor_NoLog_WhenClean` | 정상 flow 후 destruct → ERROR 로그 없음 |
| DT-02 | `Destructor_LogsError_IfStaleRetain` | 인위적으로 SelfRetain 을 null 이 아닌 상태로 destructor 진입 시 → ERROR 로그 (이 테스트 자체가 순환 참조 → reset 후 destruct, log 캡처) |

### 8.5 L3 — Stress Reproducer Scenario

| 단계 | 작업 | 기준 |
|---|---|---|
| S1 | 패치 **전** FastPortServer Debug|x64 실행 + TestClient Stress 10k×5min | UAF 크래시 재현 (예상: >=1회) |
| S2 | 패치 **전** Release|x64 로도 실행 | UAF 재현 여부 기록 (Plan Q4) |
| S3 | 패치 **후** Debug|x64 10k×5min | UAF 0회 |
| S4 | 패치 후 Release|x64 10k×5min | UAF 0회 |
| S5 | Accept 로그 count 와 ~Session 로그 count 비교 | 100% 일치 (leak 0) |
| S6 | `_CrtSetDbgFlag(_CRTDBG_LEAK_CHECK_DF)` Debug 종료 시 | 리포트 leak 0 |
| S7 | FastPortServerRIO 로 S3~S6 반복 | RIO 도 동일 결과 |

### 8.6 Regression

```bash
vstest.console.exe _Builds/x64/Debug/LibNetworksTests.dll /Platform:x64
```

기대 결과: **64 → 74 (기존 64 + LT 6 + RT 4 or DT 2) 전부 PASS, 회귀 0**.

---

## 9. Clean Architecture

### 9.1 Layer Structure (FastPort 적응)

| Layer | Responsibility | Location |
|---|---|---|
| **Protocol** | proto 직렬화 | `Protos/`, `Protocols/` (변경 없음) |
| **Session** | I/O lifecycle + lifetime retain | `LibNetworks/IOSession.*`, `LibNetworks/RIOSession.*` ← **수정** |
| **Service** | Worker thread pool, completion dispatch | `LibNetworks/IOService.*`, `LibNetworks/RIOService.*` (변경 없음) |
| **Consumer Interface** | `IIOConsumer::OnIOCompleted` | `LibNetworks/IOConsumer.ixx` (변경 없음) |
| **Test Harness** | L1 lifetime tests | `LibNetworksTests/` ← **확장** |
| **Stress Client** | TestClient Stress 탭 | `FastPortTestClient/` ← **확장** |

### 9.2 Module Dependencies

```
┌──────────────────────────────────────────────────────┐
│   LibNetworks                                        │
│   ┌────────────────────────────────────────────┐    │
│   │  IOSession / RIOSession  ←── 수정 범위      │    │
│   │   - OverlappedEx { ..., SelfRetain }        │    │
│   │   - shared_from_this() based retain         │    │
│   └────────────────────────────────────────────┘    │
│                       ↑                              │
│                       │ (변경 없음)                  │
│              IIOConsumer / IOService                 │
└──────────────────────────────────────────────────────┘
                       │
                       ↓
┌──────────────────────────────────────────────────────┐
│   FastPortServer / FastPortServerRIO                 │
│   (변경 없음 — ServiceMode / InboundSession 계약 유지)│
└──────────────────────────────────────────────────────┘
                       │
                       ↓
┌──────────────────────────────────────────────────────┐
│   FastPortTestClient                                 │
│   ┌────────────────────────────────────────────┐    │
│   │  TestClientApp (Stress tab)  ←── 수정 범위  │    │
│   │  TestRunner (Churn loop)     ←── 수정 범위  │    │
│   └────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────┘
```

---

## 10. Coding Convention Reference

프로젝트 convention 은 `CLAUDE.md` 준수:

- `PascalCase` 클래스/메서드, `m_` 멤버, `k` 상수, `::` Win32 전역 접두
- `LibCommons::Logger` 사용, GMF 에 `#include <spdlog/spdlog.h>` 필수
- 모듈 interface `.ixx` + impl `.cpp` 분리
- `std::lock_guard` / `LibCommons::RWLock`

**이 피처 특화 규약**:

| Item | Convention |
|---|---|
| SelfRetain 타입 | `std::shared_ptr<SessionType>` (IOSession 또는 RIOSession) |
| Posting site 주석 | `// Lifetime: retain for outstanding I/O — released on completion` |
| Completion site 주석 | `// Lifetime: move-out retains self until function exit` |
| 로그 카테고리 | `"IOSession"` / `"RIOSession"` (기존) + `"Stress"` (신규, TestClient 측) |

---

## 11. Implementation Guide

### 11.1 File Structure

```
LibNetworks/
├── IOSession.ixx          ← 수정: OverlappedEx::SelfRetain 추가, ~IOSession 선언
├── IOSession.cpp          ← 수정: posting (2곳) + completion (2곳) + ~IOSession
├── RIOSession.ixx         ← 수정: RequestContext::SelfRetain 추가, ~RIOSession 선언
├── RIOSession.cpp         ← 수정: posting/completion + ~RIOSession
└── (나머지 변경 없음)

LibNetworksTests/
├── IOSessionLifetimeTests.cpp   ← 신규 (LT-01~06, 6 tests)
├── RIOSessionLifetimeTests.cpp  ← 신규 (RT-01~04, 4 tests)
├── LibNetworksTests.vcxproj     ← 수정: 신규 2 파일 등록
└── LibNetworksTests.vcxproj.filters

FastPortTestClient/
├── TestClientApp.ixx      ← 수정: Stress 탭 추가
├── TestRunner.ixx         ← 수정: Churn loop (disconnect+reconnect)
└── (나머지 변경 없음)
```

### 11.2 Implementation Order

1. **Stress reproducer 먼저** (Plan Q4: Release 재현 선행) — 패치 전 UAF 재현 확인 가능해야 함.
2. **IOCP 패치** — Session 이 가장 많이 사용되는 경로, 증상 확인 즉각 가능.
3. **RIO 패치** — 동일 패턴 복제.
4. **L1 테스트** — 새 invariant 단위 검증.
5. **Stress 재실행** — 패치 후 UAF 0회 검증.

### 11.3 Session Guide

> `--scope` 파라미터로 session 분할 지원.

#### Module Map

| Module | Scope Key | Description | Estimated Turns |
|---|---|---|:-:|
| Stress reproducer | `reproducer` | TestClient Stress 탭 + Churn loop + 실측 UI | 20-25 |
| IOCP 패치 | `iocp` | `IOSession.ixx/cpp` OverlappedEx::SelfRetain + posting/completion | 15-20 |
| RIO 패치 | `rio` | `RIOSession.ixx/cpp` 동일 패턴 복제 | 15-20 |
| L1 테스트 | `tests` | IOSessionLifetimeTests + RIOSessionLifetimeTests (10 tests) | 15-20 |
| Stress 재현/검증 | `verify` | 패치 전/후 10k×5min 실행 + 로그 수집 + 결과 기록 | 10-15 |

#### Recommended Session Plan

| Session | Phase | Scope | Turns |
|---|---|---|:-:|
| Session 1 | Plan + Design | 전체 | 35-40 (완료) |
| Session 2 | Do | `--scope reproducer` | 20-25 |
| Session 3 | Do | `--scope verify` (패치 전 재현) | 10-15 |
| Session 4 | Do | `--scope iocp` | 15-20 |
| Session 5 | Do | `--scope rio` | 15-20 |
| Session 6 | Do | `--scope tests` | 15-20 |
| Session 7 | Do | `--scope verify` (패치 후 검증) | 10-15 |
| Session 8 | Check + Report | 전체 | 30-40 |

---

## Version History

| Version | Date | Changes | Author |
|---|---|---|---|
| 0.1 | 2026-04-21 | Initial draft (Option C Pragmatic 선택, Module Map 5개) | AnYounggun |
