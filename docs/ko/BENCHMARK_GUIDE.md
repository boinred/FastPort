# 벤치마크 가이드

[English](../BENCHMARK_GUIDE.md) | [한국어](BENCHMARK_GUIDE.md)

FastPort 네트워크 성능 벤치마크 도구 사용 가이드입니다.

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

### 명령줄 옵션

| 옵션 | 설명 | 기본값 |
|------|------|--------|
| `--host <ip>` | 서버 주소 | 127.0.0.1 |
| `--port <port>` | 서버 포트 | 9000 |
| `--mode <mode>` | 네트워크 모드: `iocp` 또는 `rio` | `iocp` |
| `--iterations <n>` | 반복 횟수 | 10000 |
| `--warmup <n>` | 워밍업 횟수 | 100 |
| `--payload <bytes>` | 페이로드 크기 | 64 |
| `--output <file>` | CSV 결과 파일 | - |
| `--verbose` | 상세 출력 | false |
| `--help` | 도움말 | - |

### 예시

```powershell
# 기본 테스트 (IOCP 모드)
FastPortBenchmark.exe

# RIO 모드 테스트 (고성능)
FastPortBenchmark.exe --mode rio

# 반복 횟수와 페이로드 크기 지정
FastPortBenchmark.exe --iterations 1000 --payload 256

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

- [IOCP 아키텍처](../ARCHITECTURE_IOCP.md)
- [RIO 아키텍처](../ARCHITECTURE_RIO.md)
- [패킷 프로토콜](../PACKET_PROTOCOL.md)
- [빌드 가이드](../BUILD_GUIDE.md)
