# RIO 개선 로드맵

> FastPort RIO(Registered I/O) 구현의 단계별 개선 체크리스트.
> 각 단계는 독립적으로 완료 가능하며, 우선순위 순으로 정렬되어 있다.
>
> 마지막 업데이트: 2025-02-11 (commit `301cc55` 기준)

---

## Phase 1. 핵심 인프라 수정

RIO가 정상 동작하기 위한 필수 수정 사항.

### 1.1 RIOService CQ ↔ IOCP 통합

- [ ] `RIOService::Initialize()`에서 `RIO_NOTIFICATION_COMPLETION` 구조체 생성
- [ ] `RIO_IOCP_COMPLETION` 타입으로 IOService의 IOCP 핸들에 바인딩
- [ ] 폴링 기반 `WorkerLoop()` 제거 → IOCP `GetQueuedCompletionStatus()` 기반으로 전환
- [ ] `RIONotify()` 호출로 CQ 알림 활성화 (배치 디큐 후 재등록)
- [ ] `std::this_thread::yield()` 스핀 루프 제거

> **현재 문제**: `RIOCreateCompletionQueue()`에 `nullptr` 전달 중 (`RIOService.cpp` line 30).
> IOCP에 연결되지 않아 완료 통지가 전달되지 않고, yield 기반 폴링으로 CPU를 낭비한다.
>
> **목표**: `RIONotify()`만 유일한 커널 전환이 되도록 하여, 배치 디큐의 성능 이점을 살린다.
> Accept용 IOCP 워커 풀을 재사용하여 별도 폴링 스레드를 제거할 수 있다.

### 1.2 CQ 디큐 스레드 안전성 보장

- [ ] `RIODequeueCompletion()`의 단일 스레드 접근 보장 (MS 문서: CQ 디큐는 NOT thread-safe)
- [ ] 방안 A: CQ당 전용 워커 스레드 1개 배정
- [ ] 방안 B: 여러 CQ 생성 후 스레드별 분배
- [ ] 방안 C: CQ 디큐에 뮤텍스 적용 (성능 트레이드오프 있음)

> **현재 문제**: `RIOService::Start(threadCount)`로 다수의 워커 스레드가 같은 CQ에서
> `RIODequeueCompletion()`을 동시 호출 (`RIOService.cpp` lines 77-102).
> 데이터 손상 및 미정의 동작 발생 가능.

### 1.3 RioBufferManager 슬라이스 해제/재사용

- [ ] `ReleaseSlice(RioBufferSlice&)` 메서드 추가
- [ ] Free list 기반 슬라이스 재활용 구현 (해제된 슬라이스 인덱스 스택)
- [ ] 세션 종료 시 `RIOSession` 소멸자에서 `ReleaseSlice()` 호출
- [ ] 풀 사용량 모니터링: `GetAllocatedCount()`, `GetFreeCount()` 추가
- [ ] 풀 소진 시 경고 로그 출력

> **현재 문제**: `AllocateSlice()`는 단순 오프셋 증가(pointer bump) 방식 (`RioBufferManager.cpp` line 63).
> 세션이 종료되어도 버퍼가 반환되지 않아, 장기 운영 시 풀이 고갈된다.
> 64MB 풀 기준 Recv+Send(16KB×2) → 최대 2,048 세션 후 할당 불가.

---

## Phase 2. RIOSession 완성

IOCP의 IOSession과 동등한 기능을 갖추기 위한 세션 계층 보완.

### 2.1 연결 해제(Disconnect) 처리

- [ ] `RequestDisconnect()` 메서드 구현
- [ ] 소켓 shutdown (`SD_BOTH`) 및 `closesocket()` 호출
- [ ] RQ 핸들 정리 (`m_RQ` 닫기)
- [ ] 버퍼 초기화 (`m_pReceiveBuffer->Clear()`, `m_pSendBuffer->Clear()`)
- [ ] `m_bIsDisconnected` 플래그 설정 후 추가 I/O 차단
- [ ] `OnDisconnected()` 콜백 호출
- [ ] 버퍼 슬라이스 `RioBufferManager`로 반환 (Phase 1.3 완료 후)

> **현재 문제**: `m_bIsDisconnected = true`만 설정하고 소켓, RQ, 버퍼 정리가 없다
> (`RIOSession.cpp` line 157). IOSession은 `RequestDisconnect()`에서 소켓 shutdown,
> 버퍼 clear, 에러 로깅을 모두 수행한다.

### 2.2 Send 파이프라인 안정화

- [x] 백프레셔 큐 구현 (`m_PendingSendQueue` + `m_PendingTotalBytes`, 10MB 상한)
- [x] `FlushPendingSendQueue()` 구현: 대기 큐 → 버퍼 순차 전송
- [ ] `TryPostSendFromQueue()` 다중 청크(circular buffer wrap-around) 지원
- [ ] Scatter-Gather I/O: 링 버퍼가 2개 구간으로 분할되는 경우 처리
- [ ] Send 실패 시 재시도 로직 추가
- [ ] Send 완료 후 `OnSent(size_t bytesSent)` 콜백 호출
- [ ] 백프레셔 초과 시 세션 연결 해제 (현재 로그만 출력, 주석 처리됨)

> **현재 문제**: `TryPostSendFromQueue()`가 `readBuffers[0]`만 전송 (`RIOSession.cpp` line 281-292).
> 원형 버퍼가 끝에서 시작으로 감싸는(wrap) 경우, 두 번째 청크 데이터가 누락된다.
>
> **완료된 부분**: `PendingSendQueue`와 `FlushPendingSendQueue()`로 대형 패킷 전송 흐름은 구현됨.
> Send 완료 시 큐를 다시 플러시하는 재시도 체인도 동작한다.

### 2.3 Recv 에러 복구

- [ ] `RIOReceive()` 실패 시 재시도 로직 추가
- [ ] 수신 0바이트 → 정상 종료(graceful disconnect) 처리
- [ ] 수신 에러(`bSuccess == false`) → 강제 종료(abortive disconnect) 처리
- [ ] 패킷 프레이밍 오류 시 세션 정리

> **현재 문제**: `RIOReceive()` 실패 시 아무 조치 없이 반환 (`RIOSession.cpp` line 59-74).
> 후속 Recv가 요청되지 않아 세션이 영구 정지 상태에 빠진다.
> `OnRioIOCompleted()` Recv 경로에서도 0바이트 수신 검사가 없다 (line 313-316).

### 2.4 RIOSession 소멸자 정리

- [ ] 소멸자에서 `m_RQ` 유효성 검사 후 정리
- [ ] 미완료 I/O 취소 (소켓 닫기로 인한 암묵적 취소)
- [ ] RioContext의 `pSession` 포인터 무효화 (댕글링 포인터 방지)

> **현재 문제**: 소멸자가 비어 있어 RQ 핸들이 해제되지 않는다 (`RIOSession.cpp` line 41-42).
> CQ 워커가 이미 파괴된 세션의 RioContext를 역참조할 수 있다.

### 2.5 RQ 생성 파라미터 검토

- [ ] `RIOCreateRequestQueue()` 깊이 값 검토 (현재 Send/Recv 각 1개 고정)
- [ ] 동시 outstanding I/O 수 요구사항에 맞게 조정
- [ ] RQ 생성 실패 시 세션 생성 중단 처리 (현재 로그만 출력 후 계속 진행)

> **현재 코드**: `RIOCreateRequestQueue(socket, 1, 1, 1, 1, cq, cq, this)` (`RIOSession.cpp` line 20-26).
> depth 1이면 한 번에 1개의 Send/Recv만 큐잉 가능. 부족할 수 있다.

---

## Phase 3. 애플리케이션 계층 통합

RIO 세션을 실제 서버 로직에 연결.

### 3.1 RIOInboundSession 구현

- [x] `OnPacketReceived()` 오버라이드: 패킷 ID 기반 디스패치
- [x] `HandleEchoRequest()` 구현 (기존 `FastPortInboundSession`과 동일 로직)
- [x] `HandleBenchmarkRequest()` 구현 (3-timestamp RTT 측정)
- [x] `OnAccepted()`: `SessionContainer`에 등록
- [x] `OnDisconnected()`: `SessionContainer`에서 제거
- [x] 기존 `FastPortInboundSession`과 동일한 프로토콜 지원 확인

> **완료**: `RIOInboundSession.ixx/cpp` 구현 완료 (commit `54be7e2`).
> Echo(PROTOCOL_ID_TESTS) 및 Benchmark(0x1001/0x1002) 패킷 처리 동작 확인.

### 3.2 IOSocketAcceptor RIO 모드 분기 개선

- [ ] RIO 모드 세션에 소켓 옵션(TCP_NODELAY, Keep-Alive) 설정 누락 수정
- [ ] Accept된 소켓의 `WSA_FLAG_REGISTERED_IO` 플래그 설정 확인

> **현재 문제**: `IOSocketAcceptor.cpp`에서 RIO 모드일 때 IOCP `Associate()` 호출을
> 건너뛰는 분기가 있는데, 해당 분기에서 소켓 옵션(TCP_NODELAY 등) 설정도 함께 누락된다.
> 이로 인해 RIO 세션에서 Nagle 알고리즘이 활성화되어 레이턴시가 증가할 수 있다.

---

## Phase 4. 클라이언트 & 벤치마크 RIO 지원

RIO 성능을 실제로 측정할 수 있는 환경 구축.

### 4.1 FastPortBenchmark RIO 모드

- [x] `--mode rio` 옵션 추가
- [x] `BenchmarkSessionRIO` 클래스 구현 (RIOSession 기반)
- [x] `LatencyBenchmarkRunner`에서 RIO/IOCP 모드별 서비스/세션 분기
- [x] RIO 모드 시 `RIOService` + `RioBufferManager` 초기화
- [ ] IOCP vs RIO 비교 벤치마크 결과를 하나의 리포트로 출력
- [ ] CSV 출력에 `io_mode` 컬럼 추가

> **완료**: 벤치마크 도구가 `--mode rio` 옵션으로 RIO 모드를 지원한다 (commit `ed51eab`).
> `BenchmarkSession.ixx`에 `BenchmarkSessionIOCP`와 `BenchmarkSessionRIO` 이중 구현.
> `LatencyBenchmarkRunner`가 모드에 따라 서비스/세션을 자동 생성한다.

### 4.2 RIOOutboundSession 구현

- [ ] `RIOSession` 상속 기반 범용 아웃바운드 세션 클래스 생성
- [ ] `OnConnected()` 오버라이드: 연결 완료 콜백
- [ ] `IOSocketConnector`에서 RIO 모드 세션 생성 지원
- [ ] 커넥터에 `ENetworkMode` 파라미터 전달

> **현재 상태**: `OutboundSession`은 `IOSession`(IOCP) 전용.
> 벤치마크에서는 `BenchmarkSessionRIO`가 RIOSession을 직접 상속하여 우회하고 있으나,
> 범용 RIO 아웃바운드 세션은 아직 없다.

### 4.3 FastPortClient RIO 모드

- [ ] `FastPortClient.cpp`에 `--rio` 커맨드라인 옵션 추가
- [ ] RIO 모드 시 `RIOOutboundSession` + `RIOService` 사용
- [ ] 기존 IOCP 모드와 동일한 Echo 핸드셰이크 동작 확인

---

## Phase 5. 성능 최적화

기본 동작 안정화 이후 RIO 고유의 성능 이점을 극대화.

### 5.1 Zero-Byte Recv 패턴 도입

- [ ] 유휴 세션에 대해 Zero-Byte RIOReceive 요청
- [ ] 데이터 도착 시에만 실제 버퍼 할당 후 수신
- [ ] 커널 메모리 페이지 잠금(MDL locking) 최소화

> IOSession은 Zero-Byte Recv 패턴을 사용하여 유휴 연결의 커널 리소스를 절약한다.
> RIOSession에도 동일 패턴을 적용하면 대규모 동시 연결에서 이점이 크다.

### 5.2 적응형 버퍼 풀 크기

- [ ] 동시 접속 수에 따른 풀 크기 자동 조정
- [ ] 풀 확장: 기존 풀 소진 시 추가 VirtualAlloc + RIORegisterBuffer
- [ ] 다중 BufferId 관리 (풀 확장 시 새로운 BufferId 발급)
- [ ] 커맨드라인 옵션: `--rio-pool-size`, `--rio-slot-size`

### 5.3 CQ 파티셔닝

- [ ] 워커 스레드당 전용 CQ 생성 (스레드 안전성 + 캐시 지역성)
- [ ] 세션 → CQ 할당 전략 (라운드로빈 또는 로드 밸런싱)
- [ ] CQ별 독립적인 `RIONotify()` / `RIODequeueCompletion()`

### 5.4 Send 배치 최적화

- [ ] 다수 세션의 Send 요청을 모아 한 번의 `RIONotify()`로 처리
- [ ] `RIO_MSG_DEFER` 플래그 활용: Send 요청 축적 후 일괄 커밋

---

## Phase 6. 안정성 & 운영

프로덕션 환경에서의 안정적 운영을 위한 항목.

### 6.1 에러 핸들링 강화

- [ ] `RIO_CORRUPT_CQ` 발생 시 복구 전략 (CQ 재생성 또는 서버 재시작)
- [ ] `RIOSend`/`RIOReceive` 실패 코드별 분기 처리
- [ ] RioExtension 로드 시 함수 포인터 null 검증
- [ ] StartRioMode() 초기화 실패 시 IOCP 폴백 또는 명확한 에러 메시지
- [ ] `RIOService::ProcessResult()`에서 RioContext/세션 포인터 null 검사 추가

### 6.2 모니터링 & 진단

- [x] RIO 모드 시작 시 상세 로그 (RIOService/RIOSession 로깅 강화, commit `29e2037`)
- [ ] 풀 사용률 주기적 로깅 (allocated/total 비율)
- [ ] CQ 디큐 배치 크기 통계 (평균 배치 크기, 최대 배치 크기)
- [ ] 세션당 Send/Recv 바이트 수 카운터

### 6.3 데드 코드 정리

- [ ] `RioConsumer.ixx` 제거 또는 `RIOSession`에 통합 (현재 미사용 인터페이스)
- [ ] `RIOSession.h` 스텁 헤더 필요성 검토 및 정리

### 6.4 테스트 보강

- [x] RIOService CQ 생성 및 Start/Stop 테스트 (`RioServiceTests.cpp`)
- [x] RIOSession 송수신 기본 테스트 (`RioSessionTests.cpp`)
- [x] ExternalCircleBufferQueue wrap-around 및 오버플로우 테스트
- [x] RioBufferManager 풀 초기화/할당/소진 테스트 (`RioBufferManagerTests.cpp`)
- [ ] RIOSession 전체 라이프사이클 테스트 (생성 → 연결 → 송수신 → 해제)
- [ ] RioBufferManager 슬라이스 해제/재사용 테스트
- [ ] 동시 다수 세션 스트레스 테스트
- [ ] IOCP ↔ RIO 모드 전환 회귀 테스트
- [ ] 풀 소진 후 복구 시나리오 테스트
- [ ] CQ 다중 스레드 디큐 안전성 테스트

---

## 참고: IOSession vs RIOSession 기능 비교

| 기능 | IOSession (IOCP) | RIOSession (RIO) | 상태 |
|------|------------------|-------------------|------|
| Send (WSASend / RIOSend) | O | O | 구현됨 |
| Recv (WSARecv / RIOReceive) | O | O | 구현됨 |
| 백프레셔 큐 | X | O | **RIO가 더 발전** |
| 패킷 프레이밍 | O | O | 구현됨 |
| 세션 ID 자동 발급 | O | O | 구현됨 |
| RequestDisconnect | O | X | **미구현** |
| Zero-Byte Recv | O | X | Phase 5 |
| OnSent 콜백 | O | X | **미구현** |
| 소켓 옵션 설정 | O | X | **누락** |
| 에러 복구/재시도 | O | X | **미구현** |
| Scatter-Gather Send | O | △ (단일 청크만) | **불완전** |
| 소멸자 정리 | O | X | **미구현** |

---

## 참고: 전체 진행률 요약

| Phase | 항목 수 | 완료 | 진행률 |
|-------|---------|------|--------|
| Phase 1. 핵심 인프라 수정 | 15 | 0 | 0% |
| Phase 2. RIOSession 완성 | 20 | 2 | 10% |
| Phase 3. 애플리케이션 통합 | 8 | 6 | 75% |
| Phase 4. 클라이언트 & 벤치마크 | 11 | 4 | 36% |
| Phase 5. 성능 최적화 | 11 | 0 | 0% |
| Phase 6. 안정성 & 운영 | 16 | 5 | 31% |
| **전체** | **81** | **17** | **21%** |
