# FastPort 프로젝트 구조

## 📁 디렉터리 구조

```
FastPort/
├─ docs/                          # 문서
├─ FastPortServer/                # 서버 애플리케이션
├─ FastPortClient/                # 클라이언트 애플리케이션
├─ LibNetworks/                   # 네트워크 코어 라이브러리
├─ LibCommons/                    # 공용 유틸리티 라이브러리
├─ LibCommonsTests/               # LibCommons 단위 테스트
├─ LibNetworksTests/              # LibNetworks 단위 테스트
├─ Protocols/                     # Protocol Buffers 생성 파일
├─ Protos/                        # .proto 정의 파일
└─ .idea/                         # IDE 설정 (JetBrains)
```

---

## 🖥️ FastPortServer

서버 애플리케이션 (IOCP 기반 TCP 서버)

| 파일 | 설명 |
|------|------|
| `FastPortServer.cpp` | 서버 메인 진입점 |
| `FastPortServiceMode.ixx` | Windows 서비스 모드 + 리스너 구동 |
| `FastPortInboundSession.ixx` | 서버 세션 인터페이스 |
| `FastPortInboundSession.cpp` | 서버 세션 구현 |

---

## 💻 FastPortClient

클라이언트 애플리케이션 (IOCP 기반 TCP 클라이언트)

| 파일 | 설명 |
|------|------|
| `FastPortClient.cpp` | 클라이언트 메인 진입점 |
| `FastPortOutboundSession.ixx` | 클라이언트 세션 인터페이스 |
| `FastPortOutboundSession.cpp` | 클라이언트 세션 구현 |

---

## 🌐 LibNetworks

네트워크 코어 라이브러리

### Core

| 파일 | 설명 |
|------|------|
| `Socket.ixx` | Winsock 소켓 래퍼 |
| `IOConsumer.ixx` | IOCP Completion 콜백 인터페이스 |
| `IOService.ixx` | IOCP 워커 스레드 관리 (인터페이스) |
| `IOService.cpp` | IOCP 워커 스레드 관리 (구현) |

### Listener / Connector

| 파일 | 설명 |
|------|------|
| `IOSocketListener.ixx` | AcceptEx 기반 리스너 (인터페이스) |
| `IOSocketListener.cpp` | AcceptEx 기반 리스너 (구현) |
| `IOSocketConnector.ixx` | ConnectEx 기반 커넥터 (인터페이스) |
| `IOSocketConnector.cpp` | ConnectEx 기반 커넥터 (구현) |

### Session

| 파일 | 설명 |
|------|------|
| `IOSession.ixx` | 세션 I/O 처리 (인터페이스) |
| `IOSession.cpp` | 세션 I/O 처리 (구현) |
| `InboundSession.ixx` | 서버 세션 베이스 (인터페이스) |
| `InboundSession.cpp` | 서버 세션 베이스 (구현) |
| `OutboundSession.ixx` | 클라이언트 세션 베이스 (인터페이스) |
| `OutboundSession.cpp` | 클라이언트 세션 베이스 (구현) |

### Packet

| 파일 | 설명 |
|------|------|
| `Packet.ixx` | 패킷 구조체 (헤더 + 페이로드) |
| `PacketFramer.ixx` | TCP 스트림 패킷 분리기 |

---

## 🔧 LibCommons

공용 유틸리티 라이브러리

| 파일 | 설명 |
|------|------|
| `SingleTon.ixx` | 싱글턴 템플릿 |
| `RWLock.ixx` | SRWLock 기반 Reader/Writer Lock |
| `IBuffer.ixx` | 버퍼 인터페이스 |
| `CircleBufferQueue.ixx` | 원형 버퍼 큐 (IBuffer 구현) |
| `ThreadPool.ixx` | 스레드 풀 |
| `EventListener.ixx` | 이벤트 리스너 (작업 큐) |
| `StrConverter.ixx` | 문자열 변환 유틸 |

---

## 🧪 Tests

### LibCommonsTests

| 파일 | 설명 |
|------|------|
| `CircleBufferQueueTests.cpp` | 원형 버퍼 큐 단위 테스트 |

### LibNetworksTests

| 파일 | 설명 |
|------|------|
| `LibNetworksTests.cpp` | 네트워크 라이브러리 테스트 (뼈대) |

---

## 📦 Protocols / Protos

Protocol Buffers 정의 및 생성 파일

### Protos (원본 .proto)

| 파일 | 설명 |
|------|------|
| `Commons.proto` | 공통 메시지 정의 |
| `Tests.proto` | 테스트용 메시지 정의 |

### Protocols (생성된 파일)

| 파일 | 설명 |
|------|------|
| `Tests.pb.h` | 생성된 헤더 파일 |
| `Tests.pb.cc` | 생성된 구현 파일 |
| `Commons.pb.h` | 생성된 헤더 파일 |
| `Commons.pb.cc` | 생성된 구현 파일 |

---

## ⚙️ 설정 파일

| 파일 | 설명 |
|------|------|
| `vcpkg.json` | vcpkg 패키지 매니페스트 |
| `.gitignore` | Git 무시 파일 목록 |
| `README.md` | 프로젝트 README |

---

## 🔗 Git 저장소 정보

- **로컬 경로**: `C:\Users\AnYounggun\dev\github\FastPort`
- **브랜치**: `developments`
- **원격 저장소**: https://github.com/boinred/FastPort
