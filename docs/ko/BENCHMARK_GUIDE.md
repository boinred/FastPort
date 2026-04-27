# 벤치마크 가이드

[English](../BENCHMARK_GUIDE.md) | [한국어](BENCHMARK_GUIDE.md)

FastPort 네트워크 성능 벤치마크 도구 사용 가이드입니다.

현재 최적화 기준선은 IOCP Release Windows 서비스 모드에서 1000개의 실제 TCP 세션과
4096-16384 bytes 랜덤 binary payload를 사용하는 workload입니다.

기준선 문서: [benchmark-results-05-iocp-release-1000-sessions.md](../benchmark-results-05-iocp-release-1000-sessions.md)

---

## 📊 개요

`FastPortBenchmark`는 IOCP 및 RIO(Registered I/O) 기반 네트워크 서버의 성능을 측정하는 도구입니다.

### 측정 지표

| 지표 | 설명 | 단위 |
|------|------|------|
| **Latency (RTT)** | 요청-응답 왕복 시간 | µs, ms |
| **Throughput** | 초당 처리 패킷/바이트 수 | packets/sec, MB/s |
| **P50/P90/P95/P99** | 백분위 레이턴시 | µs |
| **Std Dev** | 표준 편차 (Jitter) | µs |

---

## 🚀 사용법

### 기본 실행

```powershell
# 1. 서버 먼저 실행
.\FastPortServer.exe

# 2. 벤치마크 실행 (별도 터미널)
.\FastPortBenchmark.exe
```

### Release 서비스 모드 서버 실행

관리자 PowerShell에서 실행합니다.

```powershell
.\_Builds\x64\Release\FastPortServer.exe install
sc.exe start FastPortServerIOCP
sc.exe query FastPortServerIOCP
netstat -ano | Select-String ':6628'
```

### 명령줄 옵션

| 옵션 | 설명 | 기본값 |
|------|------|--------|
| `--host <ip>` | 서버 주소 | 127.0.0.1 |
| `--port <port>` | 서버 포트 | 9000 |
| `--mode <mode>` | 네트워크 모드: `iocp` 또는 `rio` | `iocp` |
| `--iterations <n>` | 반복 횟수 | 10000 |
| `--warmup <n>` | 워밍업 횟수 | 100 |
| `--payload <bytes>` | 페이로드 크기 | 64 |
| `--payload-min <bytes>` | 랜덤 페이로드 최소 크기 | - |
| `--payload-max <bytes>` | 랜덤 페이로드 최대 크기 | - |
| `--payload-pool <n>` | 사전 생성 payload 개수 | 1024 |
| `--sessions <n>` | 동시 TCP 세션 수 | 1 |
| `--io-threads <n>` | 클라이언트 IOCP 워커 스레드 수 | 2 |
| `--output <file>` | CSV 결과 파일 | - |
| `--verbose` | 상세 출력 | false |
| `--pause-on-exit` | Debug 빌드 종료 전 키 입력 대기 | false |
| `--help` | 도움말 | - |

### 예시

```powershell
# 기본 테스트 (IOCP 모드)
FastPortBenchmark.exe

# RIO 모드 테스트 (고성능)
FastPortBenchmark.exe --mode rio

# 반복 횟수와 페이로드 크기 지정
FastPortBenchmark.exe --iterations 1000 --payload 256

# IOCP 다중 세션 처리량 테스트
FastPortBenchmark.exe --sessions 1000 --payload-min 4096 --payload-max 16384 --iterations 100000 --warmup 10 --io-threads 8

# 원격 서버 테스트 + CSV 저장
FastPortBenchmark.exe --host 192.168.1.100 --port 9001 --output results.csv

# 상세 출력 모드
FastPortBenchmark.exe --iterations 100 --verbose
```

---

## 📁 출력 파일

### CSV 파일명 형식

출력 파일명에는 자동으로 타임스탬프가 추가됩니다.

```
results.csv → results_2024-01-15-14-30.csv
benchmark.csv → benchmark_2024-01-15-14-30.csv
```

### CSV 컬럼

| 컬럼 | 설명 |
|------|------|
| `test_name` | 테스트 이름 |
| `iterations` | 반복 횟수 |
| `payload_size` | 페이로드 크기 (bytes) |
| `avg_latency_ns` | 평균 레이턴시 (ns) |
| `min_latency_ns` | 최소 레이턴시 (ns) |
| `max_latency_ns` | 최대 레이턴시 (ns) |
| `p50_latency_ns` | P50 레이턴시 (ns) |
| `p90_latency_ns` | P90 레이턴시 (ns) |
| `p95_latency_ns` | P95 레이턴시 (ns) |
| `p99_latency_ns` | P99 레이턴시 (ns) |
| `stddev_ns` | 표준 편차 (ns) |
| `packets_per_sec` | 초당 패킷 수 |
| `mb_per_sec` | 초당 MB |
| `requested_sessions` | 요청한 세션 수 |
| `connected_sessions` | 실제 연결 완료된 세션 수 |
| `connection_losses` | 측정 중 끊긴 세션 수 |
| `warmup_requests` / `warmup_responses` | 워밍업 요청/응답 수 |
| `measured_requests` / `measured_responses` | 측정 요청/응답 수 |
| `payload_min_bytes` / `payload_max_bytes` | payload 크기 범위 |
| `payload_pool_size` | 사전 생성 payload 개수 |
| `connect_elapsed_ns` | 전체 연결 완료까지 걸린 시간 |
| `warmup_elapsed_ns` | 워밍업 구간 시간 |
| `measured_elapsed_ns` | 측정 구간 wall-clock 시간 |

---

## 📋 출력 예시

```
======================================
 FastPort Benchmark
======================================
 Server     : 127.0.0.1:9000
 Iterations : 10000
 Warmup     : 100
 Payload    : 64 bytes
======================================

Connecting to server...
Warming up...
Running benchmark...
Progress: 100% (10000/10000)

Benchmark completed!

======================================
 Benchmark: LatencyTest
======================================
 Iterations    : 10000
 Payload Size  : 64 bytes
--------------------------------------
 Latency (RTT):
   Average     : 125.32 us
   Min         : 45.20 us
   Max         : 2450.80 us
   Median      : 98.50 us
   P50         : 98.50 us
   P90         : 185.30 us
   P95         : 245.60 us
   P99         : 520.40 us
   Std Dev     : 85.20 us
--------------------------------------
 Throughput:
   Packets/sec : 7980.25
   MB/sec      : 0.49
   Total Bytes : 640000
   Elapsed     : 1253.20 ms
======================================

Results saved to: results_2024-01-15-14-30.csv
```

---

## 📈 벤치마크 시나리오

### 1. 기본 레이턴시 테스트

```powershell
FastPortBenchmark.exe --iterations 10000 --payload 64
```

소규모 패킷의 기본 RTT 측정.

### 2. 대용량 패킷 테스트

```powershell
FastPortBenchmark.exe --iterations 1000 --payload 4096
```

큰 패킷 처리 성능 측정.

### 3. 장시간 안정성 테스트

```powershell
FastPortBenchmark.exe --iterations 100000 --warmup 1000
```

장시간 운영 시 성능 변화 측정.

### 4. 비교 테스트 (IOCP vs RIO)

```powershell
# IOCP 측정
FastPortBenchmark.exe --mode iocp --output iocp_result.csv

# RIO 측정
FastPortBenchmark.exe --mode rio --output rio_result.csv

# 결과 비교 (Excel 또는 스크립트)
```

### 5. 다중 세션 IOCP 부하 테스트

```powershell
FastPortBenchmark.exe --mode iocp --sessions 1000 --payload-min 4096 --payload-max 16384 --payload-pool 4096 --iterations 100000 --warmup 10 --io-threads 8 --output iocp_load.csv
```

실제 TCP 연결 1000개를 만들고, 각 세션은 요청 1개를 보낸 뒤 응답을 받을 때마다 다음 요청을 보냅니다.
측정 중 payload 문자열은 새로 만들지 않고 시작 시 pool로 사전 생성합니다. 따라서 측정 구간에서는 payload 선택,
protobuf 직렬화, IOCP 송수신, 서버 echo 처리 비용이 주로 반영됩니다.

다중 세션 모드의 `iterations`는 전체 측정 응답 수입니다. `warmup`은 세션당 워밍업 횟수로 적용됩니다.
예를 들어 `--sessions 1000 --warmup 10`은 총 10,000개 워밍업 request/response를 통계 제외 구간으로 보냅니다.

서버의 IOCP 기본 프로필은 1000 세션 / 4K-16K echo 부하를 받을 수 있도록 세션당 recv/send 버퍼를 64KB로,
listen backlog를 1024로, 초기 AcceptEx posting 수를 256으로 설정합니다. Idle timeout은 대량 연결 warmup 중
오탐을 줄이기 위해 60초로 설정합니다.

---

## 🧪 공식 측정 정책

빠른 확인과 공식 성능 문서는 기준을 나눕니다.

| 목적 | 실행 횟수 | 용도 |
|------|----------:|------|
| Smoke check | 1회 | benchmark가 정상 완료되는지 확인 |
| 공식 benchmark | 10회 | 최적화 전후 성능 비교 문서화 |

공식 결과는 `Average RTT`, `P50`, `P99`, `Packets/sec`, `MB/sec`에 대해 best, worst, average, median,
standard deviation을 함께 기록합니다. connection loss 또는 response 누락이 있는 run은 버리지 않고 실패 run으로 기록합니다.

---

## 🔧 프로토콜 명세

### 패킷 ID

| Packet ID | 메시지 | 방향 |
|-----------|--------|------|
| `0x1001` | BenchmarkRequest | Client → Server |
| `0x1002` | BenchmarkResponse | Server → Client |

### BenchmarkRequest

```protobuf
message BenchmarkRequest {
    Header header = 1;
    uint64 client_timestamp_ns = 2;  // 클라이언트 전송 시점 (ns)
    uint32 sequence = 3;              // 시퀀스 번호
    bytes payload = 4;                // 가변 크기 페이로드
}
```

### BenchmarkResponse

```protobuf
message BenchmarkResponse {
    Header header = 1;
    ResultCode result = 2;
    uint64 client_timestamp_ns = 3;      // 클라이언트 원본 타임스탬프
    uint64 server_recv_timestamp_ns = 4; // 서버 수신 시점
    uint64 server_send_timestamp_ns = 5; // 서버 전송 시점
    uint32 sequence = 6;                 // 시퀀스 번호
    bytes payload = 7;                   // 페이로드 (Echo)
}
```

---

## ⚠️ 주의사항

### 워밍업

- 초기 연결 및 JIT 최적화를 위해 워밍업 필요
- 기본값 100회, 정밀 측정 시 1000회 이상 권장

### 네트워크 환경

- localhost 테스트 시 네트워크 지연 최소화
- 원격 테스트 시 네트워크 대역폭/지연 고려

### 시스템 부하

- 다른 프로세스의 CPU/네트워크 사용량 최소화
- 여러 번 측정 후 평균값 사용 권장

### 결과 해석

- P99 레이턴시가 평균보다 훨씬 높다면 tail latency 이슈
- Std Dev가 높다면 성능 불안정

---

## 🔗 관련 문서

- [현재 벤치마크 기준선](../benchmark-results-05-iocp-release-1000-sessions.md)
- [IOCP 아키텍처](../ARCHITECTURE_IOCP.md)
- [RIO 아키텍처](../ARCHITECTURE_RIO.md)
- [패킷 프로토콜](../PACKET_PROTOCOL.md)
- [빌드 가이드](../BUILD_GUIDE.md)
