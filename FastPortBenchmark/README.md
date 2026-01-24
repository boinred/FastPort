# FastPortBenchmark

IOCP 네트워크 성능 벤치마크 도구입니다.

## 📊 측정 항목

| 지표 | 설명 | 단위 |
|------|------|------|
| **Latency (RTT)** | 요청-응답 왕복 시간 | µs, ms |
| **Throughput** | 초당 처리 패킷/바이트 수 | packets/sec, MB/s |
| **P50/P90/P95/P99** | 백분위 레이턴시 | µs |
| **Jitter** | 표준 편차 | µs |

## 🚀 사용법

```powershell
# 기본 실행
FastPortBenchmark.exe

# 옵션 지정
FastPortBenchmark.exe --host 127.0.0.1 --port 9000 --iterations 10000

# CSV 출력
FastPortBenchmark.exe --output results.csv
```

## ⚙️ 명령줄 옵션

| 옵션 | 설명 | 기본값 |
|------|------|--------|
| `--host <ip>` | 서버 주소 | 127.0.0.1 |
| `--port <port>` | 서버 포트 | 9000 |
| `--iterations <n>` | 반복 횟수 | 10000 |
| `--warmup <n>` | 워밍업 횟수 | 100 |
| `--payload <bytes>` | 페이로드 크기 | 64 |
| `--output <file>` | CSV 결과 파일 | - |
| `--verbose` | 상세 출력 | false |

## 📋 출력 예시

```
======================================
 Benchmark: LatencyTest
======================================
 Iterations    : 10000
 Payload Size  : 64 bytes
--------------------------------------
 Latency (RTT):
   Average     : 125.32 µs
   Min         : 45.20 µs
   Max         : 2450.80 µs
   Median      : 98.50 µs
   P50         : 98.50 µs
   P90         : 185.30 µs
   P95         : 245.60 µs
   P99         : 520.40 µs
   Std Dev     : 85.20 µs
--------------------------------------
 Throughput:
   Packets/sec : 7980.25
   MB/sec      : 0.49
   Total Bytes : 640000
   Elapsed     : 1253.20 ms
======================================
```

## 🔧 서버 요구사항

서버에서 벤치마크 요청을 처리하려면:

1. 패킷 ID `0x1001` (BenchmarkRequest) 수신
2. 패킷 ID `0x1002` (BenchmarkResponse)로 Echo 응답

```cpp
void OnPacketReceived(const Core::Packet& packet) override
{
    if (packet.GetPacketId() == 0x1001)  // BENCHMARK_REQUEST
    {
        fastport::protocols::benchmark::BenchmarkRequest request;
        if (packet.ParseMessage(request))
        {
            fastport::protocols::benchmark::BenchmarkResponse response;
            response.set_client_timestamp_ns(request.client_timestamp_ns());
            response.set_sequence(request.sequence());
            response.set_payload(request.payload());
            response.set_server_recv_timestamp_ns(GetCurrentTimeNs());
            response.set_server_send_timestamp_ns(GetCurrentTimeNs());
            
            SendMessage(0x1002, response);  // BENCHMARK_RESPONSE
        }
    }
}
```

## 📁 파일 구조

```
FastPortBenchmark/
├─ FastPortBenchmark.cpp    # 메인 진입점
├─ BenchmarkSession.ixx     # 벤치마크용 세션
├─ LatencyBenchmark.cpp     # 레이턴시 측정 구현
├─ BenchmarkStats.h         # 통계 계산
├─ BenchmarkRunner.h        # 실행기 인터페이스
└─ README.md
```

## 📈 벤치마크 시나리오

### 1. 기본 레이턴시 테스트
```powershell
FastPortBenchmark.exe --iterations 10000 --payload 64
```

### 2. 대용량 패킷 테스트
```powershell
FastPortBenchmark.exe --iterations 1000 --payload 4096
```

### 3. 장시간 안정성 테스트
```powershell
FastPortBenchmark.exe --iterations 100000 --warmup 1000
```

### 4. 비교 테스트 (최적화 전후)
```powershell
# 최적화 전
FastPortBenchmark.exe --output before.csv

# 최적화 적용 후
FastPortBenchmark.exe --output after.csv

# 결과 비교 (스크립트 또는 Excel)
```
