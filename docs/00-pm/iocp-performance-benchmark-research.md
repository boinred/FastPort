# IOCP 성능 벤치마크 리서치

> 조사일: 2026-04-16  
> 목적: FastPort RIO 엔진의 성능 기준선(baseline) 확보를 위한 기존 IOCP 벤치마크 자료 수집

---

## 1. IOCP vs RIO 직접 비교 (Microsoft WinCAT 공식)

Microsoft WinCAT 팀 측정. 64바이트 TCP echo, AMD 6코어 3.2GHz.

| Windows 버전 | 소켓 방식 | Loopback Fast Path | 평균 RTT (us) | 메시지/초 | 레이턴시 감소 | 지터 감소 |
|---|---|---|---|---|---|---|
| WS 2008R2 | IOCP wait | 미사용 | **27** | **37,000** | 기준(0%) | 기준(0%) |
| WS 2008R2 | IOCP poll | 미사용 | 26 | 38,600 | 4% | 12% |
| WS 2012 | IOCP wait | 미사용 | 17 | 59,000 | 37% | 45% |
| WS 2012 | IOCP wait | 사용 | 12 | 83,000 | 55% | 45% |
| WS 2012 | RIO poll | 미사용 | 9 | 111,000 | 67% | 76% |
| WS 2012 | RIO poll | 사용 | **3** | **333,000** | **89%** | **90%** |

**핵심**: RIO + Loopback Fast Path 조합 시 IOCP 대비 레이턴시 9배 개선 (27us → 3us), 처리량 9배 향상 (37K → 333K msg/s). 지터는 +/- 319 나노초 이내.

> 출처: [Microsoft WinCAT - Fast TCP Loopback Performance](https://learn.microsoft.com/en-us/archive/blogs/wincat/fast-tcp-loopback-performance-and-low-latency-with-windows-server-2012-tcp-loopback-fast-path)

---

## 2. UDP 처리량 비교 (10GbE, ServerFramework)

8 I/O 스레드, 10 Gigabit 링크에서 UDP 데이터그램 처리.

| 서버 설계 | 처리량 (dgram/s) | 링크 사용률 | 데이터그램 성공률 | 활성 스레드 |
|---|---|---|---|---|
| 전통 IOCP (8 스레드) | **384,000** | 33% | ~99% | 8/8 |
| IOCP + GetQueuedCompletionStatusEx | 360,000 | 31% | - | - |
| RIO + IOCP (8 스레드) | **492,000** | 43% | 100% | 4/8 |
| RIO Hybrid | **501,000** | - | 98% | 8 (99%는 2스레드) |

별도 하드웨어 테스트:

| 서버 | dgram/s | 배수 |
|------|---------|------|
| 전통 IOCP | 122,000 | 1x |
| 단순 RIO | 482,000 | **~4x** |

**핵심**: RIO 서버는 전통 IOCP 대비 1.3~4배 빠르며, 컨텍스트 스위치가 현저히 적고, 유저 모드에서 더 많은 시간을 소비.

> 출처: [ServerFramework - RIO vs IOCP Performance](https://serverframework.com/asynchronousevents/2012/08/winsock-registered-io-io-completion-port-performance.html)

---

## 3. 크로스 플랫폼 I/O 모델 비교

### 3.1 아키텍처 차이

| 특성 | IOCP (Windows) | epoll (Linux) | io_uring (Linux) | kqueue (BSD) |
|---|---|---|---|---|
| 모델 | Completion (proactor) | Readiness (reactor) | Completion (proactor) | Readiness (reactor) |
| 비동기 방식 | 진정한 비동기 | reactor 패턴 | 진정한 비동기 | reactor 패턴 |
| 커널 전환 | 최소화 (배치) | 매 I/O마다 syscall | 최소화 (공유 링 버퍼) | 매 이벤트마다 syscall |
| 유저모드 폴링 | RIO로 가능 | 불가 | 가능 | 불가 |

### 3.2 IOCP vs epoll 벤치마크

Boost.Asio 1.53, Intel i7-3770k, 16GB RAM, 10,000세션 echo 서버:

| 지표 | IOCP (Windows) | epoll (Linux) |
|---|---|---|
| 처리량 | 동등 | 동등 |
| 평균 CPU (RSS NIC) | **6.8%** | **7.38%** |
| 10,000 세션 유지 | 안정적 | RSS NIC 필수 |

> 출처: [IOCP vs EPOLL Performance (SlideShare)](https://www.slideshare.net/sm9kr/iocp-vs-epoll-perfor)

### 3.3 io_uring vs epoll 레이턴시

Intel Xeon E5-2682 v4 @ 2.50GHz, 16바이트 패킷 (Alibaba Cloud):

| 지표 | epoll | io_uring (batch=1) |
|---|---|---|
| 오퍼레이션당 비용 (mitigation ON) | 2,200ns | 2,600ns |
| 오퍼레이션당 비용 (mitigation OFF) | 1,730ns | 2,130ns |
| 1,000 동시 연결 처리량 | 기준 | +10% 향상 |
| DB 워크로드 IOPS | 기준 | +30% CPU 절감, 5M+ IOPS |

**핵심**: 단일 오퍼레이션 시 io_uring이 오히려 느림. 배치 처리(batch > 1)에서 io_uring 우세.

> 출처: [Alibaba Cloud - io_uring vs epoll](https://www.alibabacloud.com/blog/io-uring-vs--epoll-which-is-better-in-network-programming_599544)

---

## 4. IOCP 확장성 (C10K / C10M)

| 동시 연결 | 상태 | 주요 병목 |
|----------|------|----------|
| 1K | 안정적, CPU < 10% | 병목 없음 |
| 10K (C10K) | IOCP로 해결 가능 | NonPaged Pool 모니터링 필요 |
| 30K+ | Accept에서 WSAENOBUFS 가능 | AcceptEx 사전 할당 필수 |
| 65K+ | IPv4 포트 번호 한계 | SO_REUSEADDR, 다중 IP 필요 |
| 100K | 가능하나 메모리 관리 중요 | I/O page lock limit 주의 |
| 1M | 이론적 가능 | 전체 리소스 최적화 필수 |

> 출처: [ServerFramework - 10K+ Connections](https://serverframework.com/asynchronousevents/2010/10/how-to-support-10000-concurrent-tcp-connections.html), [One Million TCP Connections](https://serverframework.com/asynchronousevents/2010/12/one-million-tcp-connections.html)

---

## 5. IOCP 알려진 병목

### 5.1 컨텍스트 스위칭 오버헤드

- 1,000개 동시 I/O 시 초당 수천 번의 컨텍스트 스위치 발생 가능
- RIO는 유저모드 폴링으로 대폭 감소
- 해결: 연결당 완료 버퍼 큐 + 순차 처리

### 5.2 Lock Contention

- IOCP는 어떤 스레드에서든 완료 통지 처리 → 공유 상태 접근 시 mutex 필요
- 같은 연결에 여러 스레드 동시 처리 시 경합 발생
- 해결: 연결별 완료 큐 + 순차 처리 패턴

### 5.3 스레드 풀 권장

- IOCP 동시성 = CPU 코어 수
- 총 스레드 = (코어 수 x 2) + 10~20
- 스레드가 커널 모드에서 블록되면 자동으로 다른 스레드를 깨움

> 출처: [ServerFramework - Reducing Context Switches](https://serverframework.com/asynchronousevents/2013/03/reducing-context-switches-and-increasing-performance.html)

---

## 6. 실제 서비스 사례

### 게임 서버

- 언리얼 엔진 4 네트워킹 vs 자체 IOCP 서버 성능 비교 (한국게임학회, 2019)
- GameDev.net: 700 활성 TCP 연결, 3년 된 노트북에서 200Mbit 링크 포화, CPU ~10%
- io_uring 기반 MMO 게임서버 성능개선 연구 (DBpia)

### 금융 시스템

- 초저지연 트레이딩: 커널 바이패스 NIC 사용 시 1~5 마이크로초 타겟
- 표준 NIC: 20~50 마이크로초 네트워크 레이턴시
- RIO API는 Microsoft가 "financial services trading and high speed market data" 용도로 명시

---

## 7. FastPort 벤치마크 기준선 요약

| 지표 | 전통 IOCP (기준선) | RIO (FastPort 타겟) | 기대 개선율 |
|---|---|---|---|
| TCP RTT 레이턴시 | 27 us | 3 us | **9x** |
| TCP 메시지/초 | 37,000 | 333,000 | **9x** |
| UDP dgram/초 (10GbE) | 122K~384K | 482K~501K | **1.3~4x** |
| 컨텍스트 스위치 | 높음 | 매우 낮음 | 유의미 감소 |
| 지터 | 기준 | +/- 319ns | **90% 감소** |

### 권장 벤치마크 시나리오

1. **기본 echo 테스트**: 64바이트 TCP 메시지 RTT (IOCP 기준선 27us, RIO 타겟 3us)
2. **UDP 처리량**: 10GbE에서 dgram/s 측정 (IOCP 기준 384K, RIO 타겟 500K+)
3. **동시 연결 확장성**: 1K / 10K / 100K 연결에서 CPU/메모리 사용량
4. **Linux 대비**: 동일 워크로드에서 io_uring과 비교

---

## 8. 참고 자료

### Microsoft 공식

- [RIO API Extensions](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-server-2012-r2-and-2012/hh997032(v=ws.11))
- [TCP Loopback Fast Path + RIO 벤치마크 (WinCAT)](https://learn.microsoft.com/en-us/archive/blogs/wincat/fast-tcp-loopback-performance-and-low-latency-with-windows-server-2012-tcp-loopback-fast-path)
- [I/O Completion Ports](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports)

### ServerFramework (Len Holgate)

- [RIO vs IOCP Performance](https://serverframework.com/asynchronousevents/2012/08/winsock-registered-io-io-completion-port-performance.html)
- [RIO Performance Take 2](https://serverframework.com/asynchronousevents/2012/08/windows-8server-2012-registered-io-performance---take-2.html)
- [10K+ Concurrent Connections](https://serverframework.com/asynchronousevents/2010/10/how-to-support-10000-concurrent-tcp-connections.html)
- [One Million TCP Connections](https://serverframework.com/asynchronousevents/2010/12/one-million-tcp-connections.html)

### 크로스 플랫폼

- [IOCP vs EPOLL Performance (한국어)](https://www.slideshare.net/sm9kr/iocp-vs-epoll-perfor)
- [io_uring vs epoll (Alibaba Cloud)](https://www.alibabacloud.com/blog/io-uring-vs--epoll-which-is-better-in-network-programming_599544)

### 한국어 논문/자료

- [IOCP 게임서버 성능 평가 (한국게임학회, 2019)](https://www.kci.go.kr/kciportal/ci/sereArticleSearch/ciSereArtiView.kci?sereArticleSearchBean.artiId=ART002533750)
- [io_uring MMO 게임서버 성능개선 (DBpia)](https://dbpia.co.kr/journal/articleDetail?nodeId=NODE10510511)
- [NDC12 Lockless 게임서버 설계](https://www.slideshare.net/noerror/lockless-12650474)
