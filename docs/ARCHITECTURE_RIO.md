# RIO 아키텍처

## 🏗️ 전체 아키텍처

```mermaid
graph TB
    subgraph Application["Application Layer"]
        Server[FastPortServer]
        Client[FastPortClient]
    end

    subgraph Session["Session Layer"]
        Inbound[InboundSession]
        Outbound[OutboundSession]
    end

    subgraph Network["Network Core Layer"]
        Acceptor[IOSocketAcceptor<br/>AcceptEx]
        Connector[IOSocketConnector<br/>ConnectEx]
        RIOSession[RIOSession<br/>RIOSend/RIOReceive]
        Framer[PacketFramer]
        Packet[Packet]
    end

    subgraph RIO["RIO Service Layer"]
        RIOService[RIOService<br/>Completion Queue + IOCP]
        RIOBufferPool[RIOBufferPool<br/>사전 등록 버퍼]
        RQ[Request Queue<br/>소켓별 Send/Recv 큐]
        CQ[Completion Queue<br/>완료 결과 큐]
    end

    subgraph IOCP["IOCP Service Layer"]
        IOService[IOService<br/>Worker Thread Pool]
        Consumer[IIOConsumer]
    end

    subgraph Common["Common Layer"]
        Logger[Logger]
        Lock[RWLock]
        ThreadPool[ThreadPool]
        EventListener[EventListener]
    end

    Server --> Inbound
    Client --> Outbound
    Inbound --> RIOSession
    Outbound --> RIOSession
    Acceptor --> Inbound
    Connector --> Outbound
    RIOSession --> Framer
    Framer --> Packet
    RIOSession --> RIOBufferPool
    RIOSession --> RQ
    RQ --> CQ
    CQ --> RIOService
    RIOService --> IOService
    IOService --> Consumer
    Acceptor --> IOService
    Connector --> IOService
    RIOSession --> EventListener
    EventListener --> ThreadPool
```

> **핵심 차이점**: Accept/Connect는 기존 IOCP(AcceptEx/ConnectEx)를 그대로 사용하고, 데이터 Send/Recv만 RIO로 대체합니다.

---

## 🔄 RIO 초기화 흐름

### RIO 함수 테이블 로딩

```mermaid
sequenceDiagram
    participant App as Application
    participant RIO as RIOService
    participant WS as WinSock
    participant Kernel as Windows Kernel

    App->>RIO: Initialize()
    RIO->>WS: WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP)
    Note over RIO: 임시 소켓 생성 (함수 테이블 로딩용)

    RIO->>Kernel: WSAIoctl(SIO_GET_MULTIPLE_EXTENSION_FUNCTION_POINTER<br/>WSAID_MULTIPLE_RIO)
    Kernel-->>RIO: RIO_EXTENSION_FUNCTION_TABLE 반환

    Note over RIO: 함수 포인터 획득 완료
    Note over RIO: RIORegisterBuffer<br/>RIOCreateCompletionQueue<br/>RIOCreateRequestQueue<br/>RIOReceive / RIOSend<br/>RIONotify / RIODequeueCompletion<br/>RIOCloseCompletionQueue<br/>RIODeregisterBuffer<br/>RIOResizeCompletionQueue<br/>RIOResizeRequestQueue

    RIO->>WS: closesocket(tmpSocket)
```

### Completion Queue 생성

```mermaid
sequenceDiagram
    participant RIO as RIOService
    participant IOCP as IOService
    participant Kernel as Windows Kernel

    RIO->>IOCP: CreateIoCompletionPort() 핸들 획득

    Note over RIO: IOCP 통지 모드 설정
    RIO->>RIO: RIO_NOTIFICATION_COMPLETION 구성
    Note over RIO: Type = RIO_IOCP_COMPLETION<br/>Iocp.IocpHandle = hIOCP<br/>Iocp.CompletionKey = RIO_CQ_KEY<br/>Iocp.Overlapped = &overlapped

    RIO->>Kernel: RIOCreateCompletionQueue(maxEntries, &notification)
    Kernel-->>RIO: RIO_CQ 핸들 반환

    Note over RIO: CQ가 IOCP에 연결됨
```

### 버퍼 사전 등록

```mermaid
sequenceDiagram
    participant Pool as RIOBufferPool
    participant RIO as RIO API
    participant Kernel as Windows Kernel

    Pool->>Pool: VirtualAlloc(totalSize)
    Note over Pool: 대용량 메모리 블록 할당<br/>(연속된 물리 메모리 확보)

    Pool->>RIO: RIORegisterBuffer(pBuffer, totalSize)
    RIO->>Kernel: 메모리 Pin (MDL 고정)
    Kernel-->>RIO: RIO_BUFFERID 반환

    Note over Pool: 버퍼를 슬롯 단위로 분할 관리
    Note over Pool: Slot[0]: {BufferId, Offset=0, Length=slotSize}<br/>Slot[1]: {BufferId, Offset=slotSize, Length=slotSize}<br/>Slot[N]: {BufferId, Offset=N*slotSize, Length=slotSize}

    Pool->>Pool: Free List에 모든 슬롯 등록
```

---

## 🔌 Accept 흐름 (서버)

```mermaid
sequenceDiagram
    participant Acceptor as IOSocketAcceptor
    participant IOCP as IOCP
    participant Kernel as Windows Kernel
    participant Session as RIOSession
    participant RIO as RIOService
    participant Pool as RIOBufferPool

    Note over Acceptor: Listen 소켓 생성 (기존 IOCP 방식)
    Acceptor->>IOCP: CreateIoCompletionPort(listenSocket)
    Acceptor->>Kernel: AcceptEx() 비동기 요청
    Note over Acceptor,Kernel: Pre-posted Accept (기존과 동일)

    Kernel-->>IOCP: Accept 완료 통지
    IOCP-->>Acceptor: OnIOCompleted()

    Acceptor->>Session: CreateSession() 콜백
    Note over Session: RIOSession 생성

    Session->>Pool: AllocateSlot() × 2
    Note over Session: Recv 슬롯 + Send 슬롯 확보

    Session->>RIO: RIOCreateRequestQueue(socket,<br/>maxRecv, maxSend, recvCQ, sendCQ,<br/>socketContext)
    RIO-->>Session: RIO_RQ 핸들 반환

    Session->>Session: OnAccepted()
    Session->>RIO: RIOReceive() 초기 수신 요청

    Acceptor->>Kernel: AcceptEx() 다시 요청
```

---

## 🔗 Connect 흐름 (클라이언트)

```mermaid
sequenceDiagram
    participant Connector as IOSocketConnector
    participant IOCP as IOCP
    participant Kernel as Windows Kernel
    participant Session as RIOSession
    participant RIO as RIOService
    participant Pool as RIOBufferPool

    Connector->>Connector: 소켓 생성 + bind
    Connector->>IOCP: CreateIoCompletionPort(socket)
    Connector->>Kernel: ConnectEx() 비동기 요청

    Kernel-->>IOCP: Connect 완료 통지
    IOCP-->>Connector: OnIOCompleted()

    Connector->>Session: CreateSession() 콜백
    Note over Session: RIOSession 생성

    Session->>Pool: AllocateSlot() × 2
    Note over Session: Recv 슬롯 + Send 슬롯 확보

    Session->>RIO: RIOCreateRequestQueue(socket,<br/>maxRecv, maxSend, recvCQ, sendCQ,<br/>socketContext)
    RIO-->>Session: RIO_RQ 핸들 반환

    Session->>Session: OnConnected()
    Session->>RIO: RIOReceive() 초기 수신 요청
```

---

## 📨 Send/Recv 흐름

### Recv 흐름

```mermaid
sequenceDiagram
    participant Session as RIOSession
    participant RQ as Request Queue
    participant CQ as Completion Queue
    participant IOCP as IOCP
    participant Worker as Worker Thread
    participant Pool as RIOBufferPool
    participant Framer as PacketFramer
    participant App as OnPacketReceived

    %% 1. RIOReceive 요청 (유저모드)
    Session->>Pool: AllocateSlot() → Recv 슬롯
    Note over Session: RIO_BUF 구성<br/>{BufferId, Offset, Length}
    Session->>RQ: RIOReceive(RQ, pRioBuf, 1, 0, requestContext)
    Note over Session,RQ: 유저모드 큐잉 (시스템 콜 없음!)

    Session->>CQ: RIONotify(CQ)
    Note over Session,CQ: 커널에 "새 요청 있음" 알림<br/>(유일한 시스템 콜)

    %% 2. 커널이 데이터 수신 완료
    Note over CQ: 커널이 CQ 공유 메모리에 결과 기록

    CQ-->>IOCP: IOCP 완료 통지 (IOCP 모드)
    IOCP-->>Worker: GetQueuedCompletionStatus() 반환

    %% 3. 배치 Dequeue
    Worker->>CQ: RIODequeueCompletion(CQ, results[], maxResults)
    Note over Worker,CQ: 유저모드 읽기 (시스템 콜 없음!)<br/>한 번에 여러 완료 결과 수집

    %% 4. 결과 처리
    loop 각 RIORESULT 처리
        Worker->>Session: ProcessRecvCompletion(result)
        Session->>Pool: GetSlotData(bufferId, offset)
        Note over Session: 사전 등록 버퍼에서 직접 읽기

        loop 패킷 파싱
            Session->>Framer: TryFrameFromBuffer(data)
            alt Ok
                Framer-->>Session: PacketFrame
                Session->>App: OnPacketReceived(packet)
            else NeedMore
                Framer-->>Session: NeedMore
                Note over Session: 루프 종료
            else Invalid
                Framer-->>Session: Invalid
                Session->>Session: RequestDisconnect()
            end
        end

        %% 5. 다음 Recv 요청
        Session->>RQ: RIOReceive() 다시 요청
    end

    Worker->>CQ: RIONotify(CQ)
    Note over Worker: 다음 완료 통지 대기
```

### Send 흐름

```mermaid
sequenceDiagram
    participant App as Application
    participant Session as RIOSession
    participant Pool as RIOBufferPool
    participant RQ as Request Queue
    participant CQ as Completion Queue
    participant IOCP as IOCP
    participant Worker as Worker Thread

    App->>Session: SendMessage(packetId, message)

    %% 1. 사전 등록 버퍼에 직렬화
    Note over Session, Pool: Zero-Copy Serialization
    Session->>Pool: AllocateSlot() → Send 슬롯
    Session->>Pool: Serialize directly to registered buffer
    Note over Session: RIO_BUF 구성<br/>{BufferId, Offset, Length}

    %% 2. RIOSend 요청 (유저모드)
    Session->>Session: TryPostSend()

    alt m_SendInProgress == false
        Session->>Session: m_SendInProgress = true
        Session->>RQ: RIOSend(RQ, pRioBuf, 1, 0, requestContext)
        Note over Session,RQ: 유저모드 큐잉 (시스템 콜 없음!)
        Session->>CQ: RIONotify(CQ)
    else m_SendInProgress == true
        Note over Session: 대기 (기존 Send 완료 후 처리)
    end

    %% 3. Send 완료
    Note over CQ: 커널이 CQ에 완료 기록
    CQ-->>IOCP: IOCP 완료 통지
    IOCP-->>Worker: GetQueuedCompletionStatus()

    Worker->>CQ: RIODequeueCompletion(CQ, results[], maxResults)

    Worker->>Session: ProcessSendCompletion(result)
    Session->>Pool: ReleaseSlot(sendSlot)
    Session->>Session: m_SendInProgress = false

    alt 대기 중인 Send 데이터 존재
        Session->>Session: TryPostSend()
    end
```

---

## 🔒 동기화 전략

### Completion Queue 동기화

> **중요**: `RIODequeueCompletion()`은 스레드 안전하지 않습니다. 하나의 CQ에 대해 동시에 여러 스레드가 Dequeue하면 안 됩니다.

| 전략 | 설명 |
|------|------|
| **CQ별 단일 스레드** | CQ 1개당 Worker 1개만 Dequeue (가장 간단) |
| **CQ 분리** | 소켓 그룹별 별도 CQ → 병렬 처리 가능 |
| **CriticalSection** | CQ 접근 시 락 사용 (성능 저하 가능) |

### Atomic 플래그 (IOCP와 동일 패턴)

| 플래그 | 용도 |
|--------|------|
| `m_SendInProgress` | RIOSend outstanding 1개 유지 |
| `m_DisconnectRequested` | 중복 disconnect 방지 |

### 동작 패턴

```cpp
// Send 중복 방지 (IOCP 패턴과 동일)
bool expected = false;
if (!m_SendInProgress.compare_exchange_strong(expected, true))
{
    return;  // 이미 전송 중
}

// ... RIOSend() 요청 ...

// 완료 시
m_SendInProgress.store(false);
```

### CQ Dequeue 보호

```cpp
// Worker Thread에서 CQ 접근
void OnRIOCompletionNotified(RIO_CQ cq)
{
    RIORESULT results[MAX_BATCH_SIZE];

    // RIODequeueCompletion은 스레드 안전하지 않음
    // → 하나의 CQ에 대해 단일 스레드만 호출
    ULONG numResults = g_RIO.RIODequeueCompletion(cq, results, MAX_BATCH_SIZE);

    for (ULONG i = 0; i < numResults; ++i)
    {
        auto* session = reinterpret_cast<RIOSession*>(results[i].SocketContext);
        session->OnIOCompleted(results[i]);
    }

    // 다음 통지 등록
    g_RIO.RIONotify(cq);
}
```

---

## 🧵 스레드 모델

```mermaid
graph LR
    subgraph IOCP_Workers["IOCP Worker Threads"]
        W1[Worker 1]
        W2[Worker 2]
        W3[Worker N]
    end

    subgraph CQ_Processing["CQ 처리"]
        CQ1[Completion Queue 1]
        CQ2[Completion Queue 2]
    end

    subgraph EventListener["EventListener Thread Pool"]
        E1[Event Worker 1]
        E2[Event Worker 2]
    end

    IOCP((IOCP)) --> W1
    IOCP --> W2
    IOCP --> W3

    W1 --> |RIONotify 수신| CQ1
    W2 --> |RIONotify 수신| CQ2

    CQ1 --> |RIODequeueCompletion<br/>배치 N건| Dispatch((결과 분배))
    CQ2 --> |RIODequeueCompletion<br/>배치 N건| Dispatch

    Dispatch --> |OnPacketReceived| Queue((Task Queue))

    Queue --> E1
    Queue --> E2
```

### 역할 분담

| 스레드 | 역할 |
|--------|------|
| IOCP Worker | RIONotify 완료 수신, RIODequeueCompletion 배치 처리, AcceptEx/ConnectEx 완료 처리 |
| EventListener Worker | 패킷 처리 (비즈니스 로직) |

### IOCP Worker 루프

```cpp
void WorkerThread(HANDLE hIOCP)
{
    while (true)
    {
        DWORD bytes = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED pOverlapped = nullptr;

        GetQueuedCompletionStatus(hIOCP, &bytes, &completionKey, &pOverlapped, INFINITE);

        if (completionKey == RIO_CQ_KEY)
        {
            // RIO 완료 통지
            OnRIOCompletionNotified(rioCQ);
        }
        else
        {
            // 기존 IOCP 처리 (Accept/Connect)
            OnIOCPCompleted(completionKey, bytes, pOverlapped);
        }
    }
}
```

---

## ⚡ 성능 최적화 포인트

### 1. 버퍼 사전 등록 (커널 잠금 제거)

```cpp
// 초기화 시 1회만 등록
void* pBuffer = VirtualAlloc(nullptr, TOTAL_BUFFER_SIZE,
                             MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
RIO_BUFFERID bufferId = g_RIO.RIORegisterBuffer(
    reinterpret_cast<PCHAR>(pBuffer), TOTAL_BUFFER_SIZE);

// 이후 I/O에서는 BufferId + Offset만 전달
RIO_BUF rioBuf;
rioBuf.BufferId = bufferId;
rioBuf.Offset = slotIndex * SLOT_SIZE;
rioBuf.Length = SLOT_SIZE;
```

### 2. 유저모드 큐잉 (시스템 콜 최소화)

```cpp
// RIOSend/RIOReceive는 시스템 콜 없이 공유 메모리에 기록
g_RIO.RIOReceive(rq, &rioBuf, 1, 0, requestContext);  // 유저모드!
g_RIO.RIOSend(rq, &rioBuf, 1, 0, requestContext);      // 유저모드!

// RIONotify만 시스템 콜 (커널에 "확인해라" 알림)
g_RIO.RIONotify(cq);  // 시스템 콜 1회
```

### 3. 배치 Dequeue (한 번에 N건 수집)

```cpp
// 한 번의 호출로 최대 256건 결과 수집
constexpr ULONG MAX_RESULTS = 256;
RIORESULT results[MAX_RESULTS];

ULONG numResults = g_RIO.RIODequeueCompletion(cq, results, MAX_RESULTS);

// 배치 처리 → 오버헤드 분산
for (ULONG i = 0; i < numResults; ++i)
{
    ProcessCompletion(results[i]);
}
```

### 4. VirtualAlloc 정렬 할당

```cpp
// 페이지 경계 정렬로 TLB 효율 극대화
void* pBuffer = VirtualAlloc(
    nullptr,
    TOTAL_BUFFER_SIZE,
    MEM_COMMIT | MEM_RESERVE,
    PAGE_READWRITE);
```

---

## 📦 버퍼 관리

### RIOBufferPool 구조

```mermaid
graph TB
    subgraph BufferPool["RIOBufferPool"]
        Meta[메타데이터<br/>BufferId, TotalSize, SlotSize]

        subgraph Memory["VirtualAlloc 연속 메모리"]
            S0["Slot 0<br/>Offset: 0"]
            S1["Slot 1<br/>Offset: 4096"]
            S2["Slot 2<br/>Offset: 8192"]
            SN["Slot N<br/>Offset: N×4096"]
        end

        FreeList["Free List<br/>(Lock-Free Stack)"]
    end

    subgraph RIOBuf["RIO_BUF 참조"]
        B0["BufferId + Offset=0 + Len=4096"]
        B1["BufferId + Offset=4096 + Len=4096"]
    end

    FreeList --> S0
    FreeList --> S1
    FreeList --> S2
    FreeList --> SN

    S0 -.-> B0
    S1 -.-> B1
```

### 슬롯 할당/해제

```cpp
struct RIOBufferSlot
{
    RIO_BUFFERID BufferId;
    ULONG Offset;
    ULONG Length;
    char* DataPtr;  // 직접 접근 포인터
};

class RIOBufferPool
{
    RIO_BUFFERID m_BufferId;
    char* m_pBaseAddress;
    std::stack<ULONG> m_FreeSlots;  // 사용 가능 슬롯 인덱스

    RIOBufferSlot AllocateSlot()
    {
        ULONG index = m_FreeSlots.top();
        m_FreeSlots.pop();
        return {
            .BufferId = m_BufferId,
            .Offset = index * SLOT_SIZE,
            .Length = SLOT_SIZE,
            .DataPtr = m_pBaseAddress + (index * SLOT_SIZE)
        };
    }

    void ReleaseSlot(ULONG index)
    {
        m_FreeSlots.push(index);
    }
};
```

### RIO_BUF 생성 패턴

```cpp
// 슬롯에서 RIO_BUF 구성
RIO_BUF MakeRIOBuf(const RIOBufferSlot& slot, ULONG dataLength)
{
    RIO_BUF buf;
    buf.BufferId = slot.BufferId;
    buf.Offset = slot.Offset;
    buf.Length = dataLength;
    return buf;
}
```

---

## 🔄 IOCP와 RIO 비교

### 주요 차이점

| 항목 | IOCP | RIO |
|------|------|-----|
| **Send/Recv 호출** | `WSASend/WSARecv` (시스템 콜) | `RIOSend/RIOReceive` (유저모드) |
| **버퍼 관리** | 매 I/O마다 커널 잠금/해제 | 사전 등록 1회, 이후 BufferId 참조 |
| **완료 확인** | `GetQueuedCompletionStatus` (시스템 콜) | `RIODequeueCompletion` (유저모드) |
| **배치 처리** | 1건씩 | N건 일괄 |
| **커널 전환** | 매 I/O마다 2회+ | `RIONotify` 1회만 |
| **Accept/Connect** | AcceptEx / ConnectEx | 미지원 → IOCP 사용 |
| **버퍼 타입** | `WSABUF` + 동적 메모리 | `RIO_BUF` + 사전 등록 메모리 |
| **구현 복잡도** | 중간 | 높음 (버퍼 풀 관리 필요) |

### I/O 처리 비용 비교

```
IOCP (매 I/O마다):
  ① 유저모드 → 커널모드 전환
  ② 버퍼 주소 검증 + MDL 잠금
  ③ I/O 수행
  ④ MDL 잠금 해제
  ⑤ 커널모드 → 유저모드 전환
  ⑥ 완료 통지 시스템 콜

RIO (매 I/O마다):
  ① 유저모드 큐에 엔트리 추가 (시스템 콜 없음)
  ② 커널이 사전 등록 버퍼로 직접 I/O
  ③ 유저모드 큐에서 결과 읽기 (시스템 콜 없음)
```

### RIO 선택 기준

| 상황 | 권장 |
|------|------|
| 소수의 연결, 대용량 전송 | IOCP |
| 다수의 연결, 소량 메시지 | **RIO** |
| 초저지연 요구 (μs 단위) | **RIO** |
| 높은 메시지 처리량 요구 | **RIO** |
| 단순한 구현 우선 | IOCP |
| Windows 7 이하 지원 필요 | IOCP |
