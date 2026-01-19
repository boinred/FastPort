# FastPort

FastPort는 Windows IOCP 기반으로 구현한 C++ 네트워크 서버/클라이언트 샘플 프로젝트입니다. C++20 모듈(`.ixx`)을 사용하며, 세션/IOCP/버퍼/로깅을 라이브러리 형태로 분리해두었습니다.

> 저장소 브랜치/코드는 개발 중이며, 기능은 점진적으로 확장되는 형태입니다.

---

## 주요 목표

- IOCP 기반 비동기 네트워킹 학습/실험
- Listener(Accept) / Connector(Connect) / Session(Send/Recv) 레이어 분리
- 송수신 버퍼를 인터페이스(`IBuffer`)로 분리하고 구현(`CircleBufferQueue`)을 교체 가능하도록 구성
- spdlog 기반 로깅

---

## 프로젝트 구조

대략적인 디렉터리 구성은 아래와 같습니다.

```
FastPort/
├─ FastPortServer/                 # 서버 실행 파일
│  ├─ FastPortServer.cpp           # 서버 main
│  ├─ FastPortServiceMode.ixx      # 서비스 모드(시작/종료/Shutdown) + 리스너 구동
│  ├─ FastPortInboundSession.*     # 서버 Inbound 세션 샘플
│
├─ FastPortClient/                 # 클라이언트 실행 파일
│  ├─ FastPortClient.cpp           # 클라이언트 main
│  ├─ FastPortOutboundSession.*    # 클라이언트 Outbound 세션 샘플
│
├─ LibNetworks/                    # 네트워크 라이브러리(IOCP 코어)
│  ├─ Socket.*                     # Winsock Socket 래퍼
│  ├─ IOService.*                  # IOCP 워커 스레드/Completion dispatch
│  ├─ IOConsumer.ixx               # IOCP completion 대상 인터페이스
│  ├─ IOSocketListener.*           # AcceptEx 기반 Listener
│  ├─ IOSocketConnector.*          # ConnectEx 기반 Connector
│  ├─ IOSession.*                  # 세션(WSARecv/WSASend) + 송수신 큐
│  ├─ InboundSession.*             # Accept로 생성되는 세션 베이스
│  └─ OutboundSession.*            # Connect로 생성되는 세션 베이스
│
├─ LibCommons/                     # 공용 유틸 라이브러리
│  ├─ Logger.*                     # spdlog 래핑
│  ├─ RWLock.*                     # SRWLock 기반 RWLock
│  ├─ ServiceMode.*                # Windows Service/콘솔 모드 추상화
│  ├─ IBuffer.ixx                  # 송수신 버퍼 인터페이스
│  └─ CircleBufferQueue.ixx        # 원형 버퍼 큐(IBuffer 구현)
│
├─ LibCommonsTests/                # 공용 라이브러리 테스트
│  └─ CircleBufferQueueTests.cpp
└─ LibNetworksTests/               # 네트워크 라이브러리 테스트(뼈대)
   └─ LibNetworksTests.cpp
```

---

## 주요 기능 요약

### 1) IOCP 기반 비동기 처리
- `LibNetworks::Services::IOService`
  - IOCP 생성/워크 스레드 실행
  - completion 이벤트를 `IIOConsumer::OnIOCompleted(...)`로 전달

- `LibNetworks::Core::IIOConsumer`
  - IOCP completion callback을 받는 인터페이스

### 2) AcceptEx 기반 서버 리스너
- `LibNetworks::Core::IOSocketListener`
  - `AcceptEx`로 비동기 accept
  - accept 완료 시 사용자 콜백으로 `InboundSession` 생성

### 3) ConnectEx 기반 클라이언트 커넥터
- `LibNetworks::Core::IOSocketConnector`
  - `ConnectEx`로 비동기 connect
  - connect 완료 시 사용자 콜백으로 `OutboundSession` 생성

### 4) 세션 레이어 (WSARecv/WSASend)
- `LibNetworks::Sessions::IOSession`
  - `WSARecv`/`WSASend`를 IOCP 방식으로 post
  - Recv/Send는 멤버 `OVERLAPPED`를 재사용(힙 할당 최소화)
  - Send는 `m_SendInProgress`로 outstanding 1개만 유지
  - Send/Recv 버퍼 정책은 `IBuffer`로 분리(의존성 주입)

### 5) 버퍼 추상화
- `LibCommons::Buffers::IBuffer`
  - `Write/Peek/Consume` 기반의 송수신 큐 인터페이스

- `LibCommons::Buffers::CircleBufferQueue`
  - 원형 버퍼 기반 구현체
  - 내부 동기화를 위해 `RWLock` 사용

### 6) 로깅
- `LibCommons::Logger`
  - spdlog 래핑
  - 카테고리 기반 로깅

---

## 실행 흐름(개략)

### 서버
1. `FastPortServer.cpp`에서 초기화(로깅/Winsock)
2. `FastPortServiceMode` 실행
3. `FastPortServiceMode::OnStarted()`에서 `IOSocketListener::Create(...)`
4. accept 완료 시 `FastPortInboundSession` 생성 후 `OnAccepted()` 호출

### 클라이언트
1. `FastPortClient.cpp`에서 초기화(로깅/Winsock)
2. `IOService` 시작
3. `IOSocketConnector::Create(...)`로 비동기 connect
4. connect 완료 시 `FastPortOutboundSession` 생성 후 `OnConnected()` 호출

---

## 빌드/개발 환경

- Windows (IOCP/Winsock2 사용)
- Visual Studio
- C++20 Modules (`.ixx`)
- 테스트: Microsoft C++ Unit Test Framework

---

## 확장 아이디어

- 패킷 프레이밍(길이 헤더 기반) + `m_pReceiveBuffer` 누적 파서
- Send zero-copy 개선(IBuffer에 segment/WSABUF view 제공)
- 세션 매니저/컨테이너(멀티 세션 추적, 브로드캐스트)
- graceful shutdown(소켓 close + pending IO cancel + 세션 정리)

---

## 라이선스

저장소의 라이선스 정책을 따릅니다.
