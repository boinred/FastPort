# RIO (Registered I/O) Implementation Roadmap

이 문서는 FastPort 프로젝트에 RIO(Registered I/O)를 통합하기 위한 단계별 계획을 담고 있습니다. 기존 IOCP 로직을 보존하면서 선택적으로 RIO 모드를 사용할 수 있도록 설계합니다.

## Phase 1: 기반 인프라 구축 (Foundation & Infrastructure)
- [x] **LibCommons: 모드 정의**
    - [x] `ServiceMode.ixx`에 `NetworkMode` (IOCP, RIO) Enum 추가
- [x] **LibNetworks: RIO 익스텐션 로딩**
    - [x] `RioExtension.ixx` 생성: `WSAIoctl`을 통한 RIO 함수 포인터 테이블 로딩 로직 구현
    - [x] `RIORegisterBuffer`, `RIOReceive`, `RIOSend`, `RIOCreateCompletionQueue` 등 핵심 함수 포인터 래핑
- [x] **LibNetworks: RIO 전용 구조체 정의**
    - [x] RIO 전용 `OVERLAPPED` 확장 또는 Completion 처리 구조체 설계

## Phase 2: RIO 메모리 관리 (Memory Management)
- [x] **LibNetworks: RioBufferManager 구현**
    - [x] `RIORegisterBuffer`를 사용하는 대용량 메모리 풀(Chunk) 관리자 구현
    - [x] 각 세션이 Chunk의 일부를 사용할 수 있도록 `RioBufferSlice` 클래스 구현
    - [x] RIO 전용 버퍼와 기존 `IBuffer` 간의 호환성 검토 (ExternalCircleBufferQueue 구현 완료)

## Phase 3: RIO 코어 컴포넌트 구현 (Core Components)
- [x] **LibNetworks: RIOService 구현**
    - [x] RIO Completion Queue (CQ) 생성 및 관리
    - [x] `RIODequeueCompletion`을 사용하는 별도의 워커 스레드 루프 구현
    - [ ] Polling 모드 및 Notification 모드 지원 (설정 가능하도록)
- [x] **LibNetworks: RIOSession 구현**
    - [x] 세션별 RIO Request Queue (RQ) 생성
    - [x] `RIOReceive`, `RIOSend` 기반의 비동기 송수신 로직 구현
    - [x] `PacketFramer`와 연동하여 패킷 추출 로직 통합

## Phase 4: 추상화 및 리팩토링 (Abstraction)
- [x] **LibNetworks: 세션/서비스 인터페이스 추출**
    - [x] IOCP와 RIO를 공통으로 다룰 수 있는 `INetworkService` 인터페이스 정의
    - [x] `INetworkSession` 인터페이스 정의 및 IOSession/RIOSession 적용
- [ ] **LibNetworks: 세션 팩토리 패턴 도입**
    - [x] `IOSocketAcceptor`가 실행 모드에 따라 적절한 세션 객체를 생성할 수 있도록 구조 개선
    - [ ] `IOSocketConnector` 구조 개선 (클라이언트 지원용)

## Phase 5: 애플리케이션 통합 (FastPortServer)
- [x] **FastPortServer: cxxopts 통합**
    - [x] `--rio` 커맨드 라인 인자 추가
    - [x] 인자 값에 따라 서버 구동 모드 결정 로직 추가
- [x] **FastPortServer: 조건부 초기화**
    - [x] 선택된 모드에 따라 `IOService` 또는 `RIOService` 인스턴스화 및 구동

## Phase 6: 검증 및 최적화 (Verification)
- [ ] **단위 테스트 및 통합 테스트**
    - [ ] RIO 모드에서의 패킷 송수신 정확도 검증
- [ ] **성능 벤치마크**
    - [ ] `FastPortBenchmark`를 활용하여 IOCP vs RIO 성능 비교
    - [ ] 지연 시간(Latency) 및 처리량(Throughput) 개선 수치 기록

---
## 💡 구현 원칙
1. **기존 로직 보존**: `IOSession.cpp`, `IOService.cpp` 등 기존 IOCP 코드는 수정하지 않거나 최소한으로 변경합니다.
2. **독립적 구현**: RIO 관련 코드는 별도의 파일(`.ixx`, `.cpp`)로 분리하여 모듈성을 유지합니다.
3. **명시적 전환**: 런타임 인자(`--rio`)를 통해서만 RIO 모드가 활성화되도록 합니다.
