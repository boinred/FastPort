# IOCP 아키텍처

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
        Listener[IOSocketListener<br/>AcceptEx]     
        Connector[IOSocketConnector<br/>ConnectEx]
        IOSession[IOSession<br/>WSARecv/WSASend]
        Framer[PacketFramer]
        Packet[Packet]
    end

    subgraph IOCP["IOCP Service Layer"]
        IOService[IOService<br/>Worker Thread Pool]
        Consumer[IIOConsumer]
    end

    subgraph Common["Common Layer"]
        Buffer[IBuffer / CircleBufferQueue]
        Logger[Logger]
        Lock[RWLock]
        ThreadPool[ThreadPool]
        EventListener[EventListener]
    end

    Server --> Inbound
    Client --> Outbound
    Inbound --> IOSession
    Outbound --> IOSession
    Listener --> Inbound
    Connector --> Outbound
    IOSession --> Framer
    Framer --> Packet
    IOSession --> IOService
    Listener --> IOService
    Connector --> IOService
    IOService --> Consumer
    IOSession --> Buffer
    IOSession --> EventListener
    EventListener --> ThreadPool
```

---

## 🔄 IOCP 처리 흐름

### IOService 시작

```mermaid
sequenceDiagram
    participant App as Application
    participant IO as IOService
    participant IOCP as IOCP Kernel
    participant Worker as Worker Thread

    App->>IO: Start(numThreads)
    IO->>IOCP: CreateIoCompletionPort()
    loop Worker Threads
        IO->>Worker: spawn std::thread
        Worker->>IOCP: GetQueuedCompletionStatus()
        Note over Worker,IOCP: 블로킹 대기
    end
```

### Completion 이벤트 처리

```mermaid
sequenceDiagram
    participant Kernel as Windows Kernel
    participant IOCP as IOCP
    participant Worker as Worker Thread
    participant Consumer as IIOConsumer
    participant Session as IOSession

    Kernel->>IOCP: I/O 완료 통지
    IOCP->>Worker: GetQueuedCompletionStatus() 반환
    Worker->>Consumer: OnIOCompleted(success, bytes, overlapped)
    Consumer->>Session: 실제 처리 (Recv/Send)
    Session->>Worker: 처리 완료
    Worker->>IOCP: GetQueuedCompletionStatus() (다음 대기)
```

---

## 🔌 Accept 흐름 (서버)

```mermaid
sequenceDiagram
    participant Listener as IOSocketListener
    participant IOCP as IOCP
    participant Kernel as Windows Kernel
    participant Session as InboundSession

    Note over Listener: Listen 소켓 생성
    Listener->>IOCP: CreateIoCompletionPort(listenSocket)
    Listener->>Kernel: AcceptEx() 비동기 요청
    Note over Listener,Kernel: Pre-posted Accept

    Kernel-->>IOCP: Accept 완료 통지
    IOCP-->>Listener: OnIOCompleted()
    Listener->>Session: CreateSession() 콜백
    Session->>Session: OnAccepted()
    Listener->>Kernel: AcceptEx() 다시 요청
```

---

## 🔗 Connect 흐름 (클라이언트)

```mermaid
sequenceDiagram
    participant Connector as IOSocketConnector
    participant IOCP as IOCP
    participant Kernel as Windows Kernel
    participant Session as OutboundSession

    Connector->>Connector: 소켓 생성 + bind
    Connector->>IOCP: CreateIoCompletionPort(socket)
    Connector->>Kernel: ConnectEx() 비동기 요청

    Kernel-->>IOCP: Connect 완료 통지
    IOCP-->>Connector: OnIOCompleted()
    Connector->>Session: CreateSession() 콜백
    Session->>Session: OnConnected()
```

---

## 📨 Send/Recv 흐름

### Recv 흐름

```mermaid
sequenceDiagram
    participant Session as IOSession
    participant IOCP as IOCP
    participant Kernel as Windows Kernel
    participant Buffer as ReceiveBuffer
    participant Framer as PacketFramer
    participant App as OnPacketReceived

    %% 1. Zero-byte Recv (알림 대기)
    Session->>Kernel: WSARecv(len=0) 비동기 요청 (Zero-byte)
    Note over Session: m_RecvInProgress = true (ZeroByte Mode)

    Kernel-->>IOCP: Recv 완료 통지 (0 bytes)
    IOCP-->>Session: OnIOCompleted()
    
    Note over Session: 데이터 도착 확인

    %% 2. Real Recv (데이터 수신)
    Session->>Kernel: WSARecv(Real Buffer) 비동기 요청
    Note over Session: m_RecvInProgress = true (Real Mode)

    Kernel-->>IOCP: Recv 완료 통지 (N bytes)
    IOCP-->>Session: OnIOCompleted()
    Session->>Session: m_RecvInProgress = false
    Session->>Buffer: CommitWrite(bytes)
    
    loop 패킷 파싱
        Session->>Framer: TryFrameFromBuffer(buffer)
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

    %% 3. 다시 Zero-byte Recv 대기
    Session->>Kernel: WSARecv(len=0) 비동기 요청
```

### Send 흐름

```mermaid
sequenceDiagram
    participant App as Application
    participant Session as IOSession
    participant Buffer as SendBuffer
    participant IOCP as IOCP
    participant Kernel as Windows Kernel

    App->>Session: SendMessage(packetId, message)
    
    Note over Session, Buffer: 1. Zero-Copy Serialization
    Session->>Buffer: AllocateWrite(size) -> Direct Span
    Session->>Buffer: Serialize directly to Buffer Span
    
    Session->>Session: TryPostSendFromQueue()

    alt m_SendInProgress == false
        Session->>Session: m_SendInProgress = true
        
        Note over Session, Kernel: 2. Scatter-Gather I/O
        Session->>Buffer: GetReadBuffers() -> std::vector<span<byte>>
        Session->>Session: Construct WSABUF[] from spans
        Session->>Kernel: WSASend(WSABUF[]) 비동기 요청
    else m_SendInProgress == true
        Note over Session: 대기 (기존 Send 완료 후 처리)
    end

    Kernel-->>IOCP: Send 완료 통지
    IOCP-->>Session: OnIOCompleted()
    
    Note over Session, Buffer: 3. Delayed Consume
    Session->>Buffer: Consume(bytes)
    Session->>Session: m_SendInProgress = false

    alt 버퍼에 데이터 남음
        Session->>Session: TryPostSendFromQueue()
    end
```

---

## 🔒 동기화 전략

### Atomic 플래그

| 플래그 | 용도 |
|--------|------|
| `m_RecvInProgress` | WSARecv outstanding 1개 유지 |
| `m_SendInProgress` | WSASend outstanding 1개 유지 |
| `m_DisconnectRequested` | 중복 disconnect 방지 |

### 동작 패턴

```cpp
bool expected = false;
if (!m_RecvInProgress.compare_exchange_strong(expected, true))
{
    return true;  // 이미 진행 중
}

// ... WSARecv 요청 ...

// 완료 시
m_RecvInProgress.store(false);
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

    subgraph EventListener["EventListener Thread Pool"]
        E1[Event Worker 1]
        E2[Event Worker 2]
    end

    IOCP((IOCP)) --> W1
    IOCP --> W2
    IOCP --> W3

    W1 --> |OnPacketReceived| Queue((Task Queue))
    W2 --> |OnPacketReceived| Queue
    W3 --> |OnPacketReceived| Queue

    Queue --> E1
    Queue --> E2
```

### 역할 분담

| 스레드 | 역할 |
|--------|------|
| IOCP Worker | I/O 완료 처리, 패킷 파싱 |
| EventListener Worker | 패킷 처리 (비즈니스 로직) |

---

## ⚡ 성능 최적화 포인트

### 1. OVERLAPPED 재사용
```cpp
struct OverlappedEx
{
    OVERLAPPED Overlapped{};
    std::vector<char> Buffers{};
    size_t RequestedBytes = 0;
};

// 멤버 변수로 보유 (힙 할당 최소화)
OverlappedEx m_RecvOverlapped{};
OverlappedEx m_SendOverlapped{};
```

### 2. Pre-posted Accept
```cpp
// 미리 여러 개의 AcceptEx 요청
for (int i = 0; i < PRE_ACCEPT_COUNT; ++i)
{
    PostAccept();
}
```

### 3. Outstanding I/O 제한
```cpp
// Send는 1개만 outstanding
if (!m_SendInProgress.compare_exchange_strong(expected, true))
    return;

```

### 4. Send/Recv 최적화
```cpp
// Send: 링버퍼 직접 참조 (No Intermediate Copy)
WSABUF bufs[N];
for(auto& span : ring_buffer_spans)
    bufs[i] = { len, span.data };
WSASend(bufs);

// Recv: Zero-byte Recv (No Kernel Locking for idle connections)
WSARecv(len=0); // Notification only
```
