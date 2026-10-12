# FastPort TODO

코드 분석(2026-10-12, 기준 커밋 `a5f1bd3`)에서 도출한 작업 목록입니다.
심각도: 🔴 Critical / 🟠 High / 🟡 Medium / 🟢 Low

---

## 🔴 Critical — 정합성

### [ ] 송신 링버퍼 race A: Send 완료 처리 순서로 인한 중복 송신

- 위치: `LibNetworks/IOSession.cpp` `HandleSendCompletion` — `m_SendInProgress.store(false)`(L478) 가 `m_pSendBuffer->Consume()`(L490) 보다 먼저 실행됨.
- 시나리오: 같은 세션의 Recv 완료(스레드 B)와 Send 완료(스레드 A)가 서로 다른 워커에서 동시 처리.
  A 가 플래그를 내린 직후 B 의 `SendMessage → TryPostSendFromQueue` 가 CAS 성공 →
  `GetReadBuffers` 가 아직 Consume 되지 않은 **이미 송신된 바이트**까지 포함해 `WSASend` →
  중복 송신 + 이후 `Consume` 크기 불일치(반환값 무시).
- 수정 방향: `Consume` → 누적 카운터 갱신 → 플래그 해제 순으로 변경. 플래그 해제 후 pending 재확인 로직 유지.
- 검증: Send/Recv 완료 동시 발생을 강제하는 단위 테스트 추가 (`IOSessionBytesTests` 확장).

### [ ] 송신 링버퍼 race B: 미완성 영역 송신

- 위치: `LibCommons/CircleBufferQueue.ixx` `AllocateWrite`(L147) — 예약 즉시 `m_Head`/`m_Size` 증가 ("Commit 불필요").
  호출부 `LibNetworks/IOSession.cpp` `SendMessage`(L151 이후 헤더/바디 기록).
- 시나리오: 스레드 B 가 공간 예약 후 직렬화 중, 스레드 A 의 Send 완료 경로가 `hasPending → TryPostSendFromQueue → GetReadBuffers`
  로 **아직 쓰이지 않은 바이트**를 `WSASend`.
- 수정 방향: Reserve/Commit 2단계(`AllocateWrite` → `CommitWrite`)로 분리하고, 읽기 측은 커밋된 영역만 노출.
  RIO 쪽(`LibNetworksRIO/RIOSession.cpp` L197 `CommitWrite`) 사용 패턴과 정합성 확인.
- 검증: 다중 스레드 `SendMessage` + Send 완료 동시 실행 스트레스 테스트, 수신 측 프레이밍/시퀀스 검증.

---

## 🟠 High — 안정성

### [ ] IOService: `ERROR_CONNECTION_ABORTED` 로 워커 스레드 종료

- 위치: `LibNetworks/iOService.cpp` L53 — `ERROR_ABANDONED_WAIT_0` 과 같은 분기에서 `break`.
- 영향: 개별 I/O 실패(1236)에 워커가 하나씩 소멸 + 해당 completion 이 세션으로 전달되지 않아
  outstanding 카운터 미감소 → 세션 누수(`OnDisconnected` 미발화).
- 부수: L61 의 `ERROR_CONNECTION_ABORTED` 조건은 dead code.
- 수정 방향: 종료 판정은 `pOverlapped == nullptr && ERROR_ABANDONED_WAIT_0` 로 한정. `pOverlapped != nullptr` 이면
  에러 코드와 무관하게 `OnIOCompleted(FALSE, ...)` 로 전달.

### [ ] IOSocketAcceptor: 실패 경로 누수 및 AcceptEx 슬롯 감소

- 위치: `LibNetworks/IOSocketAcceptor.cpp` L91(`UpdateAcceptContext` 실패), L112(IOService 없음) 의 조기 `return`.
- 영향: `delete pAcceptOverlapped` 누락 + `BeginAcceptEx` 재등록 누락 → 실패할 때마다 사전 AcceptEx 수가 영구 감소.
- 부수: L120 `Associate` 반환값 무시 — 실패 시 세션이 completion 을 영영 못 받음.
- 수정 방향: 실패 경로도 공통 tail(재등록 + delete)로 합류. `Associate` 실패 시 세션 정리.

### [ ] RequestDisconnect 의 즉시 closesocket

- 위치: `LibNetworks/IOSession.cpp` `RequestDisconnect` — outstanding I/O 존재 중 `Shutdown` + `Close`.
- 위험: 다른 스레드가 `WSASend`/`WSARecv` 진입 중일 때 소켓 핸들 재사용 race.
- 수정 방향: `Shutdown`(또는 `CancelIoEx`)만 먼저 하고 `closesocket` 은 outstanding == 0 시점(`TryFireOnDisconnected`)으로 이동 검토.

---

## 🟡 Medium — 프로토콜 / 리소스

### [ ] 송신 패킷 크기 오버플로우 미검사

- 위치: `LibNetworks/IOSession.cpp` L159 — `static_cast<uint16_t>(totalSize)` 무검사.
- 영향: 바디 > 65531 bytes 시 헤더 크기 잘림 → 상대편 프레이밍 붕괴.
- 수정 방향: `totalSize > 0xFFFF` 이면 에러 로그 후 송신 거부. `PACKET_PROTOCOL.md` 에 최대 크기 명시.

### [ ] PacketFramer dead code 정리

- 위치: `LibNetworks/PacketFramer.ixx` L57 — `uint16_t` 에 대한 `packetSize > 0xFFFF` 는 항상 false.
- 수정 방향: 제거하거나, 수신 버퍼 용량 기반 상한 검사로 대체.

### [ ] 미사용 Recv 버퍼 16KB 제거

- 위치: `LibNetworks/IOSession.cpp` L45 — `m_RecvOverlapped.Buffers.resize(16 * 1024)`. 실제 Recv 는 링버퍼 사용.
- 영향: 세션당 16KB 낭비 (1000 세션 ≈ 16MB).

---

## 🟢 성능 — benchmark-results-05 기준선(평균 RTT 22.56ms, 43.5K pps) 개선 후보

각 항목은 단독 A/B 로 측정해 `benchmark-results-06-*` 이후 문서로 기록한다.

### [ ] `SO_SNDBUF=0` / `SO_RCVBUF=0` 제거 A/B (최우선)

- 위치: `LibNetworks/IOSocketAcceptor.cpp` L100 → `LibNetworks/Socket.cpp` L191 `UpdateContextZeroCopy`.
- 가설: 송신 버퍼 0 + 세션당 outstanding send 1개 조합은 송신 직렬화 및 지연 ACK 상호작용으로 처리량 저하.
  수신 버퍼 0 은 TCP 수신 윈도우 축소.

### [ ] 적응형 Recv (zero-byte recv 생략)

- 위치: `LibNetworks/IOSession.cpp` L452 → L222 — 실제 수신 완료마다 zero-byte recv 재등록 → 패킷당 completion 2회.
- 방향: 직전 수신이 버퍼를 채웠거나 활성 세션이면 곧바로 real recv 등록, idle 세션에만 zero-byte recv.

### [ ] 수신 경로 할당/복사 제거

- `PacketFramer` 의 패킷당 `std::vector` 할당 + `Packet` 의 `std::string` 페이로드 복사(2회 복사).
- 벤치마크 핸들러 `set_payload(request.payload())` 추가 복사 → `mutable_payload()->swap` / `release` 검토.
- README 로드맵의 "Zero-copy receive path", "hot packet/session path object pool" 과 연계.

### [ ] 링버퍼 락 비용 검토

- `CircleBufferQueue` 모든 연산이 `RWLock` 획득. race A/B 수정 후 SPSC 특성 활용한 경량화 검토.

### [ ] 에코 핸들러 hot path 로깅

- 위치: `FastPortServer/IOCPInboundSession.cpp` L135, L155 — 패킷당 `LogInfo` 2회. `LogDebug` 로 하향.
