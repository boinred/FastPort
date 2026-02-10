# RIO (Registered I/O) Test Cases

This document outlines the test cases required to verify the integrity and performance of the RIO implementation in FastPort.

## 1. Foundation & Extension Loading
- [x] **RIO Extension Loading**: Verify that `RioExtension::Initialize` successfully loads all 9 extension functions via `WSAIoctl`.
- [x] **Double Initialization**: Ensure that calling `RioExtension::Initialize` multiple times is handled gracefully.
- [x] **Invalid Socket Handling**: Verify that initialization fails correctly when provided with an `INVALID_SOCKET`.

## 2. Memory Management (RioBufferManager)
- [x] **Pool Initialization**: Verify that `VirtualAlloc` and `RIORegisterBuffer` succeed for the requested pool size (e.g., 64MB).
- [x] **Slice Allocation**: Verify that `AllocateSlice` correctly subdivides the pool and assigns valid `RIO_BUFFERID` and offsets.
- [x] **Pool Exhaustion**: Test the behavior when the pool is completely allocated (ensure thread-safe failure or growth logic).
- [x] **Alignment Check**: Confirm that all allocated slices meet RIO's memory alignment requirements.

## 3. RIOService (Completion Queue)
- [ ] **CQ Creation**: Verify that `RIOCreateCompletionQueue` succeeds with the specified maximum results.
- [ ] **Worker Thread Dispatch**: Confirm that `RIODequeueCompletion` correctly captures completions and dispatches them to `OnRioIOCompleted`.
- [ ] **CQ Corruption Recovery**: (Advanced) Simulate or handle `RIO_CORRUPT_CQ` returns during high-load polling.
- [ ] **Graceful Stop**: Ensure that all worker threads join correctly and the CQ is closed without leaks.

## 4. RIOSession (Request Queue & Data Transfer)
- [ ] **RQ Creation**: Verify that `RIOCreateRequestQueue` succeeds for each new session.
- [ ] **Asynchronous Receive**: Confirm that `RIOReceive` is posted correctly and data is delivered to `ExternalCircleBufferQueue`.
- [ ] **Asynchronous Send**: Confirm that `RIOSend` correctly transmits data from the registered buffer slices.
- [ ] **Zero-Byte Receive Handling**: Verify that a 0-byte transfer result triggers a graceful session disconnection.
- [ ] **Send Serialization**: Test `WriteToBuffers` logic to ensure Protobuf messages are correctly spanned across RIO registered slices.
- [ ] **Outstanding Request Limits**: Ensure the session respects the `MaxReceiveResults` and `MaxSendResults` limits.

## 5. Buffer Adapter (ExternalCircleBufferQueue)
- [ ] **Wrap-around Logic**: Verify that the circular buffer correctly handles data spanning across the end of the registered memory segment.
- [ ] **Zero-Copy Integrity**: Confirm that `PacketFramer` reads directly from the RIO memory via `std::span` without intermediate copies.
- [ ] **Overflow Prevention**: Ensure `AllocateWrite` fails correctly when the RIO send slice is full.

## 6. Integration & Compatibility
- [ ] **RIO Server vs IOCP Client**: Verify full communication compatibility between a RIO-enabled server and an IOCP-based client.
- [ ] **Concurrent Sessions**: Test multiple RIO sessions (e.g., 1000+) sharing a single `RIOService`.
- [ ] **Large Packet Handling**: Verify that packets larger than a single slice (8KB) are handled (or rejected based on configuration).

---

# RIO (Registered I/O) 테스트 케이스

이 문서는 FastPort의 RIO 구현에 대한 무결성 및 성능 검증을 위한 테스트 케이스를 정의합니다.

## 1. 기반 구조 및 확장 함수 로딩
- [x] **RIO 확장 로딩**: `RioExtension::Initialize`가 `WSAIoctl`을 통해 9개의 확장 함수 포인터를 성공적으로 로드하는지 확인.
- [x] **중복 초기화**: `RioExtension::Initialize`를 여러 번 호출해도 안정적으로 처리되는지 확인.
- [x] **유효하지 않은 소켓 처리**: `INVALID_SOCKET`이 전달되었을 때 초기화 실패가 올바르게 처리되는지 확인.

## 2. 메모리 관리 (RioBufferManager)
- [x] **풀 초기화**: 요청된 풀 크기(예: 64MB)에 대해 `VirtualAlloc` 및 `RIORegisterBuffer`가 성공하는지 확인.
- [x] **슬라이스 할당**: `AllocateSlice`가 풀을 올바르게 분할하고 유효한 `RIO_BUFFERID`와 오프셋을 할당하는지 확인.
- [x] **풀 고갈**: 풀이 모두 할당되었을 때의 동작(스레드 안전한 실패 또는 확장 로직) 확인.
- [x] **정렬 확인**: 할당된 모든 슬라이스가 RIO의 메모리 정렬 요구사항을 충족하는지 확인.

## 3. RIOService (완료 큐)
- [ ] **CQ 생성**: 지정된 최대 결과 수로 `RIOCreateCompletionQueue`가 성공하는지 확인.
- [ ] **워커 스레드 디스패치**: `RIODequeueCompletion`이 완료 이벤트를 올바르게 캡처하고 `OnRioIOCompleted`로 전달하는지 확인.
- [ ] **CQ 손상 복구**: (고급) 고부하 폴링 중 `RIO_CORRUPT_CQ` 반환 시의 처리 확인.
- [ ] **정상 종료**: 모든 워커 스레드가 올바르게 종료(join)되고 CQ가 누수 없이 닫히는지 확인.

## 4. RIOSession (요청 큐 및 데이터 전송)
- [ ] **RQ 생성**: 각 세션에 대해 `RIOCreateRequestQueue`가 성공하는지 확인.
- [ ] **비동기 수신**: `RIOReceive`가 올바르게 등록되고 데이터가 `ExternalCircleBufferQueue`로 전달되는지 확인.
- [ ] **비동기 송신**: `RIOSend`가 등록된 버퍼 슬라이스에서 데이터를 올바르게 전송하는지 확인.
- [ ] **0바이트 수신 처리**: 0바이트 전송 결과가 발생했을 때 세션 연결 종료가 정상적으로 트리거되는지 확인.
- [ ] **송신 직렬화**: `WriteToBuffers` 로직이 Protobuf 메시지를 RIO 등록 슬라이스에 걸쳐 올바르게 기록하는지 확인.
- [ ] **미완료 요청 제한**: 세션이 `MaxReceiveResults` 및 `MaxSendResults` 제한을 준수하는지 확인.

## 5. 버퍼 어댑터 (ExternalCircleBufferQueue)
- [ ] **Wrap-around 로직**: 순환 버퍼가 등록된 메모리 세그먼트의 끝을 넘어가는 데이터를 올바르게 처리하는지 확인.
- [ ] **Zero-Copy 무결성**: `PacketFramer`가 중간 복사 없이 `std::span`을 통해 RIO 메모리에서 직접 읽는지 확인.
- [ ] **오버플로 방지**: RIO 송신 슬라이스가 가득 찼을 때 `AllocateWrite`가 올바르게 실패하는지 확인.

## 6. 통합 및 호환성
- [ ] **RIO 서버 vs IOCP 클라이언트**: RIO 활성화 서버와 IOCP 기반 클라이언트 간의 완전한 통신 호환성 확인.
- [ ] **동시 세션 테스트**: 단일 `RIOService`를 공유하는 다수의 RIO 세션(예: 1000개 이상) 테스트.
- [ ] **대형 패킷 처리**: 단일 슬라이스(8KB)보다 큰 패킷의 처리(또는 설정에 따른 거부) 확인.
