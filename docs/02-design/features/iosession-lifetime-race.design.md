# iosession-lifetime-race Design Document

> **Summary**: IOCP worker 의 UAF 를 **Outstanding I/O Counter + Drain-before-Remove** 패턴으로 근본 해결. Session 의 단일 owner = container. Remove 시점 = 유일한 결정적 소멸 시점. `IOSession::m_OutstandingIoCount` (atomic) 로 pending I/O 수 추적, RAII `IoCompletionGuard` 로 decrement 보장, `m_bDisconnecting` CAS 로 `shutdown+close` 멱등, `m_bOnDisconnectedFired` CAS 로 last-completion 에서 `OnDisconnected` 정확히 1회 fire. **v0.3 — SelfRetain 패턴 기각**.
>
> **Project**: FastPort
> **Author**: An Younggun
> **Date**: 2026-04-22
> **Status**: Draft (v0.3.1)
> **Planning Doc**: [iosession-lifetime-race.plan.md](../../01-plan/features/iosession-lifetime-race.plan.md) (v0.3)
> **Parent Feature**: [iocp-game-server-engine](../../01-plan/features/iocp-game-server-engine.plan.md)

---

## Context Anchor

| Key | Value |
|-----|-------|
| **WHY** | Stress 에서 confirmed UAF (2026-04-22 증거: `bResult=TRUE` 정상 completion 인데 `pConsumer=0xFFFFFFFFFFFFFFFF` — session heap + VM 페이지 반환된 깊이의 race). 상위 `iocp-game-server-engine` v1 SC-3 블로커 + M2e 선행. |
| **WHO** | FastPort 엔진 유지보수자. 간접적으로 한국 C++ 인디/미드코어 게임 서버팀. |
| **RISK** | ① Posting 실패 경로 counter 미복구 drift ② OnIOCompleted early return 에서 decrement 누락 ③ OnDisconnected 중복 호출 ④ atomic overhead 로 P99 80µs 영향 ⑤ drain 무한 지연 ⑥ shutdown(SD_BOTH) completion 미배송 ⑦ Counter Send/Recv 합쳐져 디버깅 어려움 |
| **SUCCESS** | Stress A/B/C × Debug/Release UAF 0회, 기존 64 회귀 0, 3k idle ≤3% 감소, P99 regression ≤5µs, 세션 leak 0, destruct 로그 순서 = container.Remove 이후. |
| **SCOPE** | IOCP `IOSession` 만. RIO v1.1 분리. `IIOConsumer` 불변. Counter = 단일 atomic<int>, 모든 I/O 통합. |

---

## 1. Overview

### 1.1 Design Goals

1. **Session 의 단일 owner = container** — session 이 container 에 등록되어 있는 동안만 alive. Remove 되면 즉시 `~IOSession`.
2. **Late completion 0 건** — `~IOSession` 호출 시점에는 반드시 `m_OutstandingIoCount == 0`. IOCP worker 가 freed session 에 access 하는 경로 제거.
3. **Remove 호출은 IOSession 자신이 수행** — `OnDisconnected()` 내부에서 container.Remove(sessionId). session 이 자기 여정을 스스로 마친다.
4. **RequestDisconnect 비동기 + 멱등** — admin kick / idle timeout / Error 감지 / 사용자 요청 어느 경로에서 호출해도 동일 효과. 호출자 non-blocking.
5. **RAII 로 counter 관리 누락 불가능** — `IoCompletionGuard` scope-exit 로 early return / throw 모두 자동 decrement.
6. **상위 v1 의 P99 80µs 타깃 보호** — atomic ops ≤ 10ns × 2, 무시 가능 수준으로 측정 확인.
7. **SelfRetain 패턴 명시적 기각** — 각 I/O 마다 shared_ptr 복제는 "언제 destruct 되는가" 의 단일 답을 파괴한다. 본 Design 은 그 접근을 채택하지 않는다.

### 1.2 Design Principles

1. **Lifetime invariant 를 atomic 변수 3개로 표현** — `m_OutstandingIoCount`, `m_bDisconnecting`, `m_bOnDisconnectedFired`. 세 값의 조합이 session state machine 의 유일한 진실.
2. **Exit-path 추적 금지** — `IoCompletionGuard` RAII 로 OnIOCompleted 의 어떤 return/throw 에서든 자동 decrement.
3. **Posting 실패만 inline undo** — completion 안 올 것이므로 즉시 fetch_sub 복구 + last-check.
4. **Invariant breakage 조기 발견** — `~IOSession` 에서 counter==0 assert + ERROR 로그.
5. **CAS 로 멱등성 확보** — RequestDisconnect 와 OnDisconnected fire 둘 다 `compare_exchange_strong` 기반 1회 통과.
6. **Production 코드 스타일 일관성** — CLAUDE.md 네이밍·모듈·Logger 규약 그대로.

---

## 2. Architecture Options (v1.7.0)

### 2.0 Architecture Comparison

| Criteria | Option A: Minimal inline counter | Option B: External Gate class | **Option C: IOSession-internal counter + RAII + CAS** |
|---|:-:|:-:|:-:|
| Approach | IOSession 내부에 atomic field + 각 site 수동 fetch | 별도 `PendingIoGate` 헬퍼 클래스 분리 | IOSession field + `IoCompletionGuard` RAII + helper method |
| New Files | 0 | 1 | 0 |
| Modified Files | 2 | 3 | **2** |
| Complexity | Low | Medium | **Low-Medium** |
| Maintainability | Low (누락 리스크) | High | **High (RAII + 캡슐화)** |
| Effort | Medium | High | **Low** |
| Risk | High (수동 decrement 누락) | Low (과설계) | **Low** |

**Selected**: **Option C — IOSession-internal counter + RAII `IoCompletionGuard` + CAS**

**Rationale**: Session state 는 session 자신이 소유해야 하므로 외부 Gate 분리는 과설계. 수동 fetch 는 OnIOCompleted 의 6~10개 exit path 에서 누락 리스크. RAII scope-exit 로 decrement 를 한 곳에 고정 + `m_bDisconnecting` / `m_bOnDisconnectedFired` CAS 조합으로 멱등성 확보. v0.3 Plan 과 1:1.

### 2.1 Component Diagram

```
┌────────────────────────────────────────────────────────────────────┐
│         Target IOCP Session Lifetime Path (post-activation)        │
│                                                                    │
│  Activation entry (out of main drain path):                        │
│   - Inbound : Accept 완료 → OnAccepted() → container handoff       │
│   - Outbound: ConnectEx 완료 → OnConnected() → container handoff   │
│                                                                    │
│  IOService worker                                                  │
│  ──────────────                                                    │
│  GetQueuedCompletionStatus → (completion key = raw IIOConsumer*)   │
│      ↓                                                             │
│  session-path completion 인 경우                                   │
│      ↓                                                             │
│  IOSession::OnIOCompleted                                          │
│    └ IoCompletionGuard guard{*this}   ← scope-exit decrement       │
│      └ fetch_sub(1) → prev==1 && disconnectRequested               │
│                                   ↓                                │
│                           TryFireOnDisconnected()                  │
│                                   ↓                                │
│                      m_bOnDisconnectedFired CAS                    │
│                                   ↓                                │
│                      OnDisconnected() (virtual)                    │
│                                   ↓                                │
│                      container.Remove(sessionId)                   │
│                                   ↓                                │
│                      container owner release                       │
│                                   ↓                                │
│                      ~IOSession() (deterministic)                  │
│                                                                    │
│  IOSession::RequestRecv / TryPostSendFromQueue                     │
│    ├─ fetch_add(1)                                                 │
│    ├─ ::WSARecv / ::WSASend(...)                                   │
│    └─ 실패(non-PENDING) 시 fetch_sub(1) + 복구                     │
│                                                                    │
│  IOSession::RequestDisconnect (idempotent, async)                  │
│    ├─ disconnectRequested CAS (first pass only)                    │
│    ├─ ::shutdown(SD_BOTH)                                          │
│    ├─ ::closesocket()                                              │
│    └─ return; remaining completion drain 은 worker path 담당       │
└────────────────────────────────────────────────────────────────────┘

[RIO 경로 — 본 v1 에서는 변경 없음. v1.1 `riosession-lifetime-race` 로 분리]
```

### 2.2 State Transition

```
┌─────────────────────────────────────────────────────────────────────┐
│                      IOSession Life-cycle States                    │
│                                                                     │
│  state = (OutstandingIoCount, DisconnectRequested, OnDisconnectedFired) │
│                                                                     │
│  S0: PreActivation     (0, false, false)                            │
│       │ inbound : accept 완료                                       │
│       │ outbound: connect 완료 + transport activation               │
│       ↓                                                             │
│  S1: Activated         (0, false, false)                            │
│       │ container handoff 완료                                      │
│       │ StartReceiveLoop / 앱 트래픽 시작 가능                      │
│       ↓                                                             │
│  S2: Active            (N>=0, false, false)                         │
│       │ posting ↔ completion cycle                                  │
│       │ RequestDisconnect(admin / idle / error / user)              │
│       ↓                                                             │
│  S3: DisconnectRequested (N>=0, true, false)                        │
│       │ shutdown+close 시작                                          │
│       │ N>0 이면 pending completion drain                           │
│       │ N==0 이면 zero-pending fast path                            │
│       ↓                                                             │
│  S4: DisconnectReady   (0, true, false)                             │
│       │ TryFireOnDisconnected()                                     │
│       │ m_bOnDisconnectedFired CAS (first pass)                     │
│       ↓                                                             │
│  S5: FiringOnDisc      (0, true, true)                              │
│       │ OnDisconnected() 호출 → container.Remove(id)                │
│       ↓                                                             │
│  S6: Removed           [container 의 shared_ptr drop]                │
│       │ external ref 없음                                           │
│       ↓                                                             │
│  S7: Destructed        [~IOSession() 실행 → assert count==0]         │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.2.1 Operational Lifecycle Contract

위 state machine 은 단순히 `counter` 변화만 설명하는 그림이 아니라, **owner / readiness /
disconnect 책임**을 함께 잠그는 운영 계약으로 해석해야 한다.

#### A. Ownership Handoff

| Phase | Primary Owner | Notes |
|---|---|---|
| Constructed / Pre-activation | acceptor / connector / caller | 아직 container 소유가 아니다. 이 구간은 transport 활성화 준비 단계다. |
| Activated | container | Inbound 는 `OnAccepted()`, Outbound 는 `OnConnected()` 시점에 `shared_from_this()` 로 container 에 handoff 한다. |
| Disconnecting / Draining | container | owner 는 여전히 container 하나다. pending I/O 만 drain 한다. |
| Removed | none | `OnDisconnected() -> container.Remove(id)` 이후 마지막 shared_ptr 이 drop 되면 즉시 `~IOSession()` 으로 간다. |

**Rule O1**: steady-state owner 는 항상 container 하나다. acceptor / connector / factory 가
hand-off 이후 reference 를 오래 쥐고 있으면 deterministic destruction 이 흐려진다.

**Rule O2**: `OnDisconnected()` 는 "정리 콜백"이 아니라 **유일한 제거 진입점**으로 취급한다.
이 경로 밖에서 임의 Remove / erase 를 호출하는 설계는 금지한다.

#### B. Readiness Is Separate From Lifetime

`alive` 와 `ready-to-send/recv` 는 같은 개념이 아니다.

| Axis | Meaning | Example |
|---|---|---|
| Lifetime | 객체가 아직 유효하고 container 가 owner 인가 | `m_OutstandingIoCount`, `m_DisconnectRequested`, `m_bOnDisconnectedFired` 조합 |
| Readiness | transport activation 이 완료되어 앱 트래픽을 시작해도 되는가 | Outbound 는 `ConnectEx` 완료 + `UpdateConnectContext` + `OnConnected()` 이후 |

**Rule R1**: outbound session 은 `OnConnected()` 이전에 앱 payload 송신을 시작하면 안 된다.
연결 완료 전 send 가능성은 lifetime 문제가 아니라 readiness 위반이다.

**Rule R2**: inbound / outbound 모두 `StartReceiveLoop()` 시작 시점은 activation 완료 후로
고정한다. "객체가 살아있다"는 이유만으로 송수신 가능 상태로 간주하지 않는다.

#### C. Disconnect Contract

`RequestDisconnect()` 는 단순 close helper 가 아니라, session 을 `Active -> Disconnecting`
으로 보내는 **유일한 상태 전이 API**다.

| Phase | Allowed Work | Forbidden Work |
|---|---|---|
| Active (`disconnect=false`) | 새 recv/send posting, packet 처리, 상위 feature 트래픽 시작 | 없음 |
| Disconnecting (`disconnect=true`, `outstanding>0`) | 기존 pending I/O completion drain, final fire 대기 | 새 logical work 시작, 새 app-level send loop 시작, 임의 re-arm |
| Drained (`disconnect=true`, `outstanding==0`) | `TryFireOnDisconnected()` 1회 | 멤버 재접근, 재등록, 재연결 시도 |

**Rule D1**: `m_DisconnectRequested == true` 이후에는 feature 코드가 새 logical work 를 시작하면
안 된다. Echo loop, timer callback, retry send, 추가 recv arm 모두 금지다.

**Rule D2**: posting 함수가 disconnect 이후 잘못 호출되더라도, 결과는 "즉시 실패 + counter
복구"여야 한다. 이를 성공 경로처럼 취급해 session lifetime 을 연장하면 안 된다.

**Rule D3**: `TryFireOnDisconnected()` 를 호출하는 모든 경로는 그 호출을 **마지막 동작**으로
간주해야 한다. 호출 이후 `this` 접근 가능성을 전제한 코드는 금지한다.

#### D. Practical Mapping In This Repository

- Inbound: acceptor 가 세션을 만들고 `OnAccepted()` 에서 container handoff 후 정상 운용에
  진입한다.
- Outbound: connector / caller 가 connect 완료 전까지 임시 owner 이고, `OnConnected()` 에서
  container handoff 후에만 일반 트래픽을 시작한다.
- 공통: `RequestDisconnect()` 는 이유와 무관하게 동일한 drain 경로로 합류하고, 마지막
  completion 또는 pending-zero fast path 에서만 `OnDisconnected()` 가 fire 된다.

### 2.3 Dependencies

| Component | Depends On | Purpose |
|---|---|---|
| `IOSession` | `std::atomic<int>`, `std::atomic<bool>` | Counter + flags |
| `IoCompletionGuard` (신규, 파일 내부) | `IOSession&` | RAII decrement |
| `IOService` | (변경 없음) | 기존 completion 경로 유지 |
| `IIOConsumer` | (변경 없음) | 인터페이스 불변 |
| `IOCPInboundSession::OnDisconnected` | container weak_ref 또는 callback | Remove 호출 경로 유지 |
| `LibCommons::Logger` | (변경 없음) | 로깅 |
| **Parent v1 관계** | 본 feature 먼저 main 병합 → M2e (Recv 재설계) | M2e 는 "OnIOCompleted 진입시 session alive" 불변식 위에 올림 |

---

## 3. Data Model

### 3.1 IOSession 멤버 추가

```cpp
// IOSession.ixx — 기존 protected/private 블록에 추가
private:
    // [NEW v0.3] Outstanding I/O counter.
    // 의미: 현재 pending (posting 성공 후, completion 전) 인 I/O 개수.
    //  - posting 성공 시 +1, completion 진입 시 -1 (RAII guard)
    //  - posting 실패(non-PENDING) 시 즉시 -1 으로 복구
    //  - counter == 0 && m_bDisconnecting == true 전이 순간이 last-completion
    std::atomic<int> m_OutstandingIoCount { 0 };

    // [NEW v0.3] Disconnecting 진입 멱등 CAS.
    // RequestDisconnect 의 첫 호출만 true 로 전환 + shutdown+close 실행.
    std::atomic<bool> m_bDisconnecting { false };

    // [NEW v0.3] OnDisconnected fire 멱등 CAS.
    // 마지막 completion 에서 TryFireOnDisconnected() 가 CAS 시도 → 첫 호출만 OnDisconnected() 진입.
    std::atomic<bool> m_bOnDisconnectedFired { false };

protected:
    // [NEW v0.3] helper — counter 가 1→0 전이 && disc==true 일 때 호출.
    // 내부에서 m_bOnDisconnectedFired CAS → 통과 시 OnDisconnected() 호출.
    void TryFireOnDisconnected();
```

### 3.2 `IoCompletionGuard` RAII (익명 namespace, `IOSession.cpp`)

```cpp
// IOSession.cpp — 익명 namespace
namespace {
    // OnIOCompleted 의 어떤 exit 에서도 m_OutstandingIoCount 를 감소시키고
    // last-completion 이면 TryFireOnDisconnected 를 호출하는 RAII helper.
    struct IoCompletionGuard {
        IOSession& self;
        explicit IoCompletionGuard(IOSession& s) noexcept : self(s) {}
        ~IoCompletionGuard() noexcept {
            const int prev = self.m_OutstandingIoCount.fetch_sub(1, std::memory_order_acq_rel);
            if (prev == 1 && self.m_bDisconnecting.load(std::memory_order_acquire)) {
                self.TryFireOnDisconnected();
            }
        }
        IoCompletionGuard(const IoCompletionGuard&) = delete;
        IoCompletionGuard& operator=(const IoCompletionGuard&) = delete;
    };
}
```

> `IoCompletionGuard` 가 `IOSession` 의 private member 에 접근해야 하므로, `IOSession` 에 `friend struct IoCompletionGuard;` 를 선언하거나 helper 를 `IOSession` 의 nested protected struct 로 배치. 본 Design 은 **nested protected struct** 를 권장 — anonymous namespace 사용 시 friend 선언 필요 → 캡슐화 경계 혼란. `IOSession::IoCompletionGuard` 로 한다.

### 3.3 Invariants

| ID | Invariant | 보장 방법 |
|---|---|---|
| I1 | `m_OutstandingIoCount >= 0` 항상 | posting 성공 시 +1, completion 시 -1 (RAII 1:1 대응) + posting 실패 시 +1 후 즉시 -1 복구 |
| I2 | 모든 pending I/O 에 대해 Windows 가 completion 배송 보장 | IOCP semantics (`WSA_IO_PENDING` 또는 `SOCKET_ERROR != WSA_IO_PENDING`). `closesocket()` 후에도 fail completion 배송 보장 |
| I3 | OnIOCompleted 진입 시 counter 는 항상 >= 1 | posting 성공했으므로 fetch_add 완료 상태 |
| I4 | `RequestDisconnect` 는 정확히 1회만 shutdown+close 실행 | disconnectRequested CAS 첫 통과 시에만 |
| I5 | `OnDisconnected()` 는 정확히 1회만 호출 | `m_bOnDisconnectedFired.compare_exchange_strong(false, true)` 통과 시에만 |
| I6 | Last-completion 조건: `fetch_sub(1)` prev == 1 && disconnectRequested == true | RAII guard 에서 체크 |
| I7 | `~IOSession` 진입 시 `m_OutstandingIoCount == 0` | container 가 유일한 owner, Remove 는 last-completion 이후에만 |
| I8 | OnDisconnected 호출은 항상 counter == 0 인 상태에서 발생 | I6 로 조건 성립 확인 후에만 fire |
| I9 | Outbound app traffic 는 connect readiness 이후에만 시작 | `OnConnected()` 이전에는 payload send 금지 |
| I10 | disconnectRequested == true 이후에는 새 logical work 시작 금지 | feature/timer/retry 계층 contract 로 보장 |

---

## 4. API Specification

> REST 없음. Internal C++ API contracts.

### 4.1 Endpoint List (공개/내부 API 요약)

| # | API | 구분 | 역할 |
|---|---|---|---|
| 1 | `IOSession::RequestRecv(bool bZeroByte)` | protected | Recv posting (counter +1) |
| 2 | `IOSession::TryPostSendFromQueue()` | protected | Send posting (counter +1) |
| 3 | `IOSession::OnIOCompleted(bool, DWORD, OVERLAPPED*)` | override (`IIOConsumer`) | IOCP worker dispatch (RAII counter -1) |
| 4 | `IOSession::RequestDisconnect()` | public | **비동기 멱등** disconnect. shutdown+close 후 즉시 return |
| 5 | `IOSession::OnDisconnected()` | virtual, derived override | container.Remove 수행 (호출은 last-completion 에서만 1회) |
| 6 | `IOSession::TryFireOnDisconnected()` | protected | CAS 기반 OnDisconnected 1회 fire |
| 7 | `~IOSession()` | destructor | counter==0 assert + 로그 |

### 4.2 Posting Pattern

```cpp
// IOCP RequestRecv — zero-byte / real 공통
bool IOSession::RequestRecv(bool bZeroByte)
{
    m_RecvOverlapped.ResetOverlapped();
    m_RecvOverlapped.IsZeroByte = bZeroByte;
    m_RecvOverlapped.WSABufs.clear();

    // ... WSABufs 구성 (기존 로직) ...

    // [NEW v0.3] posting 직전 counter +1
    m_OutstandingIoCount.fetch_add(1, std::memory_order_acq_rel);

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
            // [NEW v0.3] completion 안 올 것 → inline undo + last-check
            UndoOutstandingOnFailure("RequestRecv");
            LibCommons::Logger::GetInstance().LogError("IOSession",
                std::format("RequestRecv() WSARecv failed. Session Id : {}, Error Code : {}, ZeroByte : {}",
                    GetSessionId(), err, bZeroByte));
            return false;
        }
    }
    return true;
}

// [NEW v0.3] helper — posting 실패 시 counter 복구 + last-check
void IOSession::UndoOutstandingOnFailure(const char* site)
{
    const int prev = m_OutstandingIoCount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1 && m_bDisconnecting.load(std::memory_order_acquire))
    {
        TryFireOnDisconnected();
    }
    (void)site; // Debug log 에 활용 가능
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

    // [NEW v0.3] posting 직전 counter +1
    m_OutstandingIoCount.fetch_add(1, std::memory_order_acq_rel);

    int result = ::WSASend(..., &m_SendOverlapped.Overlapped, nullptr);
    if (result == SOCKET_ERROR)
    {
        int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            // [NEW v0.3] inline undo + last-check
            UndoOutstandingOnFailure("TryPostSendFromQueue");
            m_SendInProgress.store(false);
            return false;
        }
    }
    return true;
}
```

### 4.3 Completion Pattern — RAII guard

```cpp
void IOSession::OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped)
{
    if (!pOverlapped) return;

    // [NEW v0.3] 진입 즉시 RAII guard 생성.
    //   - 함수 어느 exit 에서든 ~IoCompletionGuard 가 fetch_sub(1) 후 last-check.
    //   - early return, throw 모두 안전.
    IoCompletionGuard guard{ *this };

    if (pOverlapped == &(m_RecvOverlapped.Overlapped))
    {
        // ----- 기존 Recv 로직 100% 그대로 -----
        if (!bSuccess) {
            m_RecvInProgress.store(false);
            LibCommons::Logger::GetInstance().LogInfo("IOSession",
                std::format("OnIOCompleted() Recv failed. Session Id : {}, Error Code : {}",
                    GetSessionId(), ::GetLastError()));
            RequestDisconnect();  // 멱등 — 이미 disconnecting 이면 no-op
            return;
        }

        if (m_RecvOverlapped.IsZeroByte) {
            if (bytesTransferred == 0) {
                // Zero-byte completion 후 real Recv 재posting
                // (guard 의 fetch_sub 는 여기에서도 동작 — 새 posting 이 fetch_add 로 별도 +1)
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
            // Real Recv 경로 (CommitWrite, fetch_add bytes, ReadReceivedBuffers, RequestReceived)
            // ... 기존 로직 ...
            return;
        }
    }

    if (pOverlapped == &(m_SendOverlapped.Overlapped))
    {
        // ----- 기존 Send 로직 그대로 -----
        m_SendInProgress.store(false);
        if (!bSuccess) { RequestDisconnect(); return; }
        // ... Consume, fetch_add, OnSent, hasPending 재posting ...
        return;
    }
}
```

**Zero-byte → Real Recv 경로의 counter 흐름**:
```
fn entry: counter=N (was zero-byte posted, so N>=1)
  IoCompletionGuard created (scope-exit will fetch_sub)
  if bytesTransferred==0 → RequestRecv(false) posted
    → fetch_add(1): counter=N+1
    → WSARecv posted OK
  return
scope exit: ~IoCompletionGuard → fetch_sub(1): counter=N
```
각 I/O 독립적으로 +1/-1 → net 0 이지만 "이 completion 은 처리 끝, 저 posting 은 새로 시작" 이 정확히 표현됨.

### 4.4 RequestDisconnect (비동기 멱등)

```cpp
void IOSession::RequestDisconnect()
{
    bool expected = false;
    if (!m_bDisconnecting.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        // 이미 disconnecting — no-op
        LibCommons::Logger::GetInstance().LogDebug("IOSession",
            std::format("RequestDisconnect() idempotent skip. Session Id : {}", GetSessionId()));
        return;
    }

    // First pass only reach here.
    LibCommons::Logger::GetInstance().LogInfo("IOSession",
        std::format("RequestDisconnect() initiated. Session Id : {}, OutstandingIo : {}",
            GetSessionId(),
            m_OutstandingIoCount.load(std::memory_order_acquire)));

    // shutdown+close: Windows 가 모든 pending I/O 에 fail completion 배송 보장
    if (m_pSocket)
    {
        ::shutdown(m_pSocket->GetSocket(), SD_BOTH);
        m_pSocket->Close();   // 기존 Socket wrapper 의 closesocket 래퍼
    }

    // 주의: 여기서 대기하지 않음. 호출자 즉시 return.
    // Pending I/O 가 남아있다면 각 fail completion 이 OnIOCompleted 를 타고 들어와
    // IoCompletionGuard 가 counter 를 1→0 으로 내릴 때 TryFireOnDisconnected 가 호출됨.
    // Pending I/O 가 이미 0이면 이 함수 진입 시점에 이미 counter==0 이므로 아래 즉시 fire.
    if (m_OutstandingIoCount.load(std::memory_order_acquire) == 0)
    {
        TryFireOnDisconnected();
    }
}
```

> **주의**: `RequestDisconnect` 진입 시점에 pending I/O 가 없는 경우(아주 드묾 — accept 직후 disconnect 요청 같은) 도 존재. 이 때는 마지막 completion 이 없으므로 여기서 직접 `TryFireOnDisconnected()` 를 한 번 더 시도한다. CAS 덕분에 중복 호출 안전.

### 4.5 TryFireOnDisconnected (last-completion 경로)

```cpp
void IOSession::TryFireOnDisconnected()
{
    // 전제: counter 가 0 에 도달, disconnecting == true.
    // CAS 로 fired 플래그를 한 번만 통과시킴.
    bool expected = false;
    if (!m_bOnDisconnectedFired.compare_exchange_strong(
            expected, true,
            std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        // 이미 fire 된 상태 — 누군가 먼저 호출. no-op.
        return;
    }

    LibCommons::Logger::GetInstance().LogInfo("IOSession",
        std::format("TryFireOnDisconnected() firing. Session Id : {}", GetSessionId()));

    // virtual dispatch → IOCPInboundSession::OnDisconnected → container.Remove(id)
    OnDisconnected();
    // OnDisconnected 반환 시점엔 이미 container 에서 제거됐을 가능성 있음.
    // 즉 외부 shared_ptr 이 drop 되어 *this 가 소멸 중일 수 있다.
    // 이 함수 반환 후 아무 멤버도 터치하지 말 것. (안전을 위해 return 직후 종료)
}
```

> **위험 포인트**: `OnDisconnected()` 가 container.Remove 를 호출하면, container 의 shared_ptr 이 drop 되면서 이 IOSession 의 destructor 가 즉시 실행될 수 있다. `TryFireOnDisconnected()` 호출 후 `OnIOCompleted` 나 `UndoOutstandingOnFailure` 등 호출부가 멤버를 추가 접근하면 UAF. 따라서 **호출부에서 `TryFireOnDisconnected()` 호출은 반드시 함수의 "마지막 동작" 으로 배치**. RAII guard 의 경우 destructor 마지막 줄이므로 안전. `UndoOutstandingOnFailure` 도 `return` 직전에만 호출.

### 4.6 Destructor Paranoid Check

```cpp
IOSession::~IOSession()
{
    const int finalCount = m_OutstandingIoCount.load(std::memory_order_acquire);
    const bool wasDisc   = m_bDisconnecting.load(std::memory_order_acquire);
    const bool wasFired  = m_bOnDisconnectedFired.load(std::memory_order_acquire);

    if (finalCount != 0)
    {
        // 논리상 절대 발생 안 해야 함 — invariant 위반 시 즉시 발견.
        LibCommons::Logger::GetInstance().LogError("IOSession",
            std::format("~IOSession() INVARIANT VIOLATION — outstanding IO at destruction. "
                        "Session Id : {}, count : {}, disc : {}, fired : {}",
                        GetSessionId(), finalCount, wasDisc, wasFired));
    }
    else
    {
        LibCommons::Logger::GetInstance().LogDebug("IOSession",
            std::format("~IOSession() Session Id : {}, disc : {}, fired : {}",
                        GetSessionId(), wasDisc, wasFired));
    }
}
```

---

## 5. UI/UX Design

### 5.1 Stress Mode UI (v0.2 에서 이미 구현 완료)

`FastPortTestClient` Stress 탭 — Scenario A/B/C 라디오, RIO 라디오 disabled. 본 v0.3 에서 **변경 없음**.

### 5.2 Live stats 표시 (기존 유지)

- Scenario A: Connect Attempts / Failures / Total Accepted / Active / Churned / Elapsed / Crash detected
- Scenario B: Current Round / Packets Received / Elapsed
- Scenario C: Packets Received / Elapsed

### 5.3 Page UI Checklist

v0.2 와 동일 (변경 없음). reproducer 완료 보고 참조.

---

## 6. Error Handling

### 6.1 Error Scenarios

| # | Scenario | 발생 경로 | 처리 방식 |
|---|---|---|---|
| E1 | WSARecv immediate failure (e.g. WSAENOTSOCK) | `RequestRecv` posting | `UndoOutstandingOnFailure` → LogError → return false → caller 가 RequestDisconnect |
| E2 | WSASend immediate failure | `TryPostSendFromQueue` | 동일 — counter undo + return false |
| E3 | OnIOCompleted 내부 예외 throw | 사용자 콜백 또는 내부 로직 오류 | `IoCompletionGuard` destructor 는 noexcept fetch_sub 수행 → counter 정상화. 예외는 IOService worker 가 caller 로 전파 (기존 정책 유지) |
| E4 | RequestDisconnect 중복 호출 | admin + idle + error 동시 | CAS 첫 통과만 shutdown+close, 이후는 DEBUG 로그로 skip |
| E5 | Disconnecting 도중 새 posting 시도 | 외부 코드 오용 | fetch_add(1) 은 수행되지만 WSARecv 가 closed socket 에 fail → UndoOutstandingOnFailure 로 복구. Counter drift 없음 |
| E6 | Pending I/O 없는데 RequestDisconnect | 드문 케이스 (accept 직후) | RequestDisconnect 내부에서 counter==0 검사 후 즉시 `TryFireOnDisconnected` 호출 |
| E7 | OnDisconnected 중복 호출 시도 | worker 경합 | `m_bOnDisconnectedFired` CAS 로 정확히 1회만 |
| E8 | `~IOSession` 시 counter != 0 | 논리 버그 | ERROR 로그로 즉시 발견 |

### 6.2 Logging Contract

| Category | Level | When |
|---|---|---|
| `"IOSession"` | INFO | RequestDisconnect 첫 통과, TryFireOnDisconnected firing, session 정상 destruct |
| `"IOSession"` | DEBUG | RequestDisconnect 중복 호출 skip, destruct 상세 |
| `"IOSession"` | ERROR | WSARecv/WSASend immediate failure, `~IOSession` invariant 위반 |
| `"Stress"` | INFO/ERROR | (reproducer 측 기존) |

---

## 7. Security Considerations

- [x] 입력 검증: 변경 없음
- [x] 인증/인가: 변경 없음
- [x] 민감 정보 암호화: 변경 없음
- [x] 네트워크: 변경 없음
- [x] Rate limiting: 변경 없음

본 feature 는 내부 lifetime 관리만 변경. 외부 공격면 변화 없음.

---

## 8. Test Plan (v2.3.0)

### 8.1 Test Scope

| Type | Target | Tool | Phase |
|------|--------|------|-------|
| L1 Unit | IOSession counter/disc/fired life-cycle | `LibNetworksTests` | Do |
| L1 Unit | RequestDisconnect 멱등성 | `LibNetworksTests` | Do |
| L1 Unit | OnDisconnected 1회 fire CAS | `LibNetworksTests` | Do |
| L1 Unit | Destructor paranoid check | `LibNetworksTests` | Do |
| L3 Stress | Scenario A/B/C UAF 0 + leak 0 | `FastPortTestClient` Stress | Do (verify-after) |
| Regression | 기존 64 tests | vstest.console | Check |
| Perf Baseline | 3k idle + 64B P99 전/후 비교 | `FastPortBenchmark` | Do (verify-after) |

### 8.2 L1 — Lifetime Tests (`IOSessionLifetimeTests.cpp`)

| # | Test | Assertion |
|---|------|-----------|
| LT-01 | `PostingSuccess_CounterIncremented` | `RequestRecv(true)` 호출 후 `m_OutstandingIoCount` == 1 (테스트용 inspector) |
| LT-02 | `CompletionDecrements` | 시뮬 OnIOCompleted 후 counter == 0 |
| LT-03 | `PostingFailure_CounterUndone` | 소켓 INVALID 상태에서 `RequestRecv` → false 반환, counter 여전히 0 |
| LT-04 | `RequestDisconnect_Idempotent` | 10 회 호출 → `shutdown`/`closesocket` 호출 수 각각 1 (mock Socket) |
| LT-05 | `LastCompletionFiresOnDisconnected` | Disconnect 후 pending 완료 → OnDisconnected 콜백 1회, 호출 시점에 counter==0 |
| LT-06 | `MultipleIOs_FireAfterAllDrained` | Recv + Send 동시 posting → Disconnect → 두 completion 모두 처리 후 OnDisconnected 1회 |
| LT-07 | `OnDisconnected_FiresExactlyOnce_ConcurrentLast` | 2 worker 가 동시에 마지막 completion 처리 시도 → CAS 에 의해 OnDisconnected 콜백 정확히 1회 |

### 8.3 L1 — Destructor Paranoid

| # | Test | Assertion |
|---|------|-----------|
| DT-01 | `Destructor_NoErrorLog_WhenClean` | 정상 flow 후 destruct → ERROR 로그 없음 |
| DT-02 | `Destructor_ErrorLog_WhenStaleCounter` | 인위적으로 counter 를 !=0 으로 세팅하여 destructor 진입 → ERROR 로그 캡처 (테스트 후 counter 리셋하여 실제 UAF 회피) |

### 8.4 L3 — Stress (reproducer 재활용, verify-after 에서 실행)

| 단계 | 시나리오 | 환경 | 기준 |
|---|---|---|---|
| S1 | Scenario A — 패치 **후** | Debug\|x64 | UAF 0회, churn 30k 완료, destruct=accept 일치 |
| S2 | Scenario A — 패치 후 | Release\|x64 | 동일 |
| S3 | Scenario B (1M × 2 round) — 패치 후 | Debug\|x64 | UAF 0회, 2M packet, leak 0 |
| S4 | Scenario B — 패치 후 | Release\|x64 | 동일 |
| S5 | Scenario C (1k × 100pps × 5min) — 패치 후 | Debug\|x64 | UAF 0회, leak 0 |
| S6 | Scenario C — 패치 후 | Release\|x64 | 동일 |
| S7 | `_CrtSetDbgFlag(_CRTDBG_LEAK_CHECK_DF)` 전 시나리오 종료 시 | Debug | leak 0 |
| S8 | 3k conn idle throughput 전/후 | Release | 감소 ≤ 3% |
| S9 | 64B echo P50/P99 전/후 | Release | P99 regression ≤ 5µs |
| S10 | destruct 로그 순서 확인 | Debug | 항상 container.Remove 이후에 `~IOSession` |

### 8.5 Regression

```bash
vstest.console.exe _Builds/x64/Debug/LibNetworksTests.dll /Platform:x64
```

기대: **64 → 73 (기존 64 + LT 7 + DT 2) 전부 PASS, 회귀 0**.

---

## 9. Clean Architecture

### 9.1 Layer Structure (FastPort 적응)

| Layer | Responsibility | Location |
|---|---|---|
| **Protocol** | proto 직렬화 | `Protos/`, `Protocols/` (변경 없음) |
| **Session** | I/O lifecycle + **결정적 수명 관리 (counter + drain-before-remove)** | `LibNetworks/IOSession.*` ← **수정** |
| **Service** | Worker thread pool, completion dispatch | `LibNetworks/IOService.*` (변경 없음) |
| **Consumer Interface** | `IIOConsumer::OnIOCompleted` | `LibNetworks/IOConsumer.ixx` (변경 없음) |
| **Inbound Session** | container.Remove 호출 (`OnDisconnected` override) | `FastPortServer/IOCPInboundSession.*` (변경 없음 — 기존 OnDisconnected 로직 재사용) |
| **Test Harness** | L1 lifetime tests | `LibNetworksTests/` ← **확장** |
| **Stress Client** | Scenario A/B/C runner | `FastPortTestClient/` (완료, 변경 없음) |
| **Freeze** | v1 기간 변경 금지 | `LibNetworksRIO/`, `FastPortServerRIO/`, `LibNetworksRIOTests/` |

### 9.2 Module Dependencies

```
┌──────────────────────────────────────────────────────┐
│   LibNetworks (IOCP 축 v1)                           │
│   ┌────────────────────────────────────────────┐    │
│   │  IOSession  ←── 본 feature 수정 범위         │    │
│   │   - atomic<int> m_OutstandingIoCount        │    │
│   │   - atomic<bool> m_bDisconnecting           │    │
│   │   - atomic<bool> m_bOnDisconnectedFired     │    │
│   │   - IoCompletionGuard (nested)              │    │
│   │   - TryFireOnDisconnected()                 │    │
│   │   - UndoOutstandingOnFailure()              │    │
│   └────────────────────────────────────────────┘    │
│                       ↑                              │
│                       │ (변경 없음)                  │
│              IIOConsumer / IOService                 │
│                                                      │
│   LibNetworksRIO  ──── v1 freeze, 변경 없음         │
└──────────────────────────────────────────────────────┘
                       │
                       ↓
┌──────────────────────────────────────────────────────┐
│   FastPortServer (IOCP v1)                           │
│   IOCPInboundSession::OnDisconnected                 │
│     └ 기존 container.Remove(id) 로직 유지            │
└──────────────────────────────────────────────────────┘
```

---

## 10. Coding Convention Reference

프로젝트 convention 은 `CLAUDE.md` 준수:

- `PascalCase` / `m_` / `k` / `::` Win32
- `LibCommons::Logger`, GMF `#include <spdlog/spdlog.h>`
- 모듈 `.ixx` + `.cpp`
- `std::lock_guard` / RAII 선호

**이 피처 특화 규약**:

| Item | Convention |
|---|---|
| Counter 타입 | `std::atomic<int>` (non-negative, `int` 로 충분 — pending 수 현실상 <1000) |
| Flag 타입 | `std::atomic<bool>` + `compare_exchange_strong` |
| Memory order | posting/completion 에 `memory_order_acq_rel`, pure load 에 `memory_order_acquire` |
| RAII helper 이름 | `IOSession::IoCompletionGuard` (nested) |
| Posting site 주석 | `// Lifetime: +1 — release on completion (RAII) or undo on failure` |
| Completion site 주석 | `// Lifetime: IoCompletionGuard auto -1 on any exit` |
| 로그 카테고리 | `"IOSession"` (기존 유지) |
| 문자열 포맷 | `std::format` |

---

## 11. Implementation Guide

### 11.1 File Structure

```
LibNetworks/
├── IOSession.ixx          ← 수정: 3개 atomic 멤버 + TryFireOnDisconnected/UndoOutstandingOnFailure 선언
├── IOSession.cpp          ← 수정: posting counter, OnIOCompleted RAII, RequestDisconnect CAS, TryFire, ~IOSession
└── (나머지 변경 없음)

LibNetworks/RIOSession.*   ← v1 freeze (v1.1 이관)

LibNetworksTests/
├── IOSessionLifetimeTests.cpp   ← 신규 (LT-01~07 + DT-01~02, 9 tests)
├── LibNetworksTests.vcxproj     ← 수정
└── LibNetworksTests.vcxproj.filters

FastPortTestClient/        ← 이미 완료 (reproducer scope), 변경 없음
```

### 11.2 Implementation Order

1. **IOSession.ixx 선언 추가** — atomic 3개, helper 2개 (TryFireOnDisconnected, UndoOutstandingOnFailure), nested `IoCompletionGuard` struct, `~IOSession` 선언
2. **IOSession.cpp 구현**:
   a. `IoCompletionGuard` 정의 (nested struct)
   b. `UndoOutstandingOnFailure` 정의
   c. `RequestRecv`: fetch_add + 실패 시 UndoOutstandingOnFailure
   d. `TryPostSendFromQueue`: 동일
   e. `OnIOCompleted`: 진입 즉시 `IoCompletionGuard guard{*this}` 생성, 나머지 기존 로직
   f. `RequestDisconnect`: CAS + shutdown+close + (pending 없으면 TryFire)
   g. `TryFireOnDisconnected`: CAS + OnDisconnected() 호출 (마지막 동작)
   h. `~IOSession`: counter==0 assert + 로그
3. **Build 검증** — Debug+Release warning-as-error green
4. **L1 테스트** — IOSessionLifetimeTests.cpp (LT-01~07 + DT-01~02)
5. **`verify-after` 진입** — Scenario A/B/C + Perf

### 11.3 Session Guide

#### Module Map (v0.3 갱신)

| Module | Scope Key | Description | Estimated Turns |
|---|---|---|:-:|
| Stress reproducer (완료) | `reproducer` | Scenario A/B/C runner | (완료) |
| 패치 전 재현 증거 (완료) | `verify-before` | Debug 재현, evidence 문서 | (완료) |
| IOCP 패치 | `iocp-patch` | IOSession counter/disc/fired + RAII guard + CAS 멱등 + ~IOSession | 20-25 |
| L1 테스트 | `tests` | IOSessionLifetimeTests.cpp (LT 7 + DT 2 = 9 tests) | 15-20 |
| Stress + Perf 검증 | `verify-after` | A/B/C × Debug/Release + 3k idle + 64B P99 | 15-20 |

#### Recommended Session Plan (갱신)

| Session | Phase | Scope | Turns |
|---|---|---|:-:|
| S1 (done) | Plan v0.3 + Design v0.3 | 전체 | 25-30 |
| S2 (done) | Do `--scope reproducer` | | 25 |
| S3 (done) | Do `--scope verify-before` | | 8 |
| S4 | Do `--scope iocp-patch` | | 20-25 |
| S5 | Do `--scope tests` | | 15-20 |
| S6 | Do `--scope verify-after` | | 15-20 |
| S7 | Check + Report | 전체 | 30-40 |

---

## Version History

| Version | Date | Changes | Author |
|---|---|---|---|
| 0.1 | 2026-04-21 | Initial draft (Option C Pragmatic 선택, Module Map 5개, IOCP+RIO 동시, SelfRetain) | AnYounggun |
| 0.2 | 2026-04-22 | Plan v0.2 정렬 (RIO 분리, Stress 3 시나리오, parent feature 참조, move-out vs RetainGuard 등가) — **SelfRetain 기반 유지** | An Younggun |
| **0.3** | **2026-04-22** | **Plan v0.3 정렬 — SelfRetain 패턴 기각. Outstanding I/O Counter + Drain-before-Remove 채택. 단일 owner(container) 원칙. 3개 atomic 멤버(`m_OutstandingIoCount`, `m_bDisconnecting`, `m_bOnDisconnectedFired`). RAII `IoCompletionGuard` (nested). `RequestDisconnect` 비동기 멱등 CAS. `TryFireOnDisconnected` CAS 1회 fire. Zero-byte / Real Recv counter 독립. State machine 다이어그램 §2.2 신설. `UndoOutstandingOnFailure` helper. Destructor paranoid assert. L1 tests LT-01~07 + DT-01~02 (9 tests).** | **An Younggun** |
| **0.3.1** | **2026-04-22** | **Lifecycle contract 명문화. owner handoff(acceptor/connector -> container), readiness와 lifetime 분리, `RequestDisconnect` 이후 새 logical work 금지 규칙, inbound/outbound 실운영 매핑 추가.** | **An Younggun** |
