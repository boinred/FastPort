# FastPort

[English](./README.md) | [한국어](./README.ko.md)

**Windows IOCP 및 RIO 기반 고성능 비동기 네트워크 프레임워크**

FastPort는 C++20 모듈 기반 네트워킹 프로젝트입니다. 현재 최적화 기준선은 IOCP Release Windows 서비스 모드에서 1000개의 실제 TCP 세션과 4K-16K 랜덤 binary payload를 사용하는 benchmark입니다.

---

## 프로젝트 개요

| 항목 | 내용 |
|------|------|
| 목표 | Windows 고성능 네트워킹 프레임워크 구현 및 최적화 |
| 주 엔진 | IOCP |
| 보조 엔진 | RIO, 별도 RIO 프로젝트에서 관리 |
| 언어 | C++20 modules (`.ixx`) |
| 플랫폼 | Windows x64 |
| 툴체인 | Visual Studio / MSBuild |

---

## 기술 스택

| 분류 | 기술 |
|------|------|
| 비동기 I/O | Windows IOCP, AcceptEx, ConnectEx |
| RIO | `LibNetworksRIO`, `FastPortServerRIO` |
| 직렬화 | Protocol Buffers |
| 로깅 | spdlog 래퍼인 `LibCommons::Logger` |
| CLI | cxxopts |
| 패키지 관리 | vcpkg |

---

## 핵심 구현 내용

### IOCP 엔진

- `AcceptEx`, `ConnectEx` 기반 비동기 연결 처리.
- IOCP completion port 기반 worker thread 처리.
- idle session 효율을 위한 zero-byte receive.
- 세션당 one-outstanding-send 방식의 순차 송신.
- `WSABUF` 기반 scatter/gather send path.
- session idle checker 및 server status/admin 지원.

### Benchmark

- `FastPortBenchmark`를 통한 실제 다중 세션 benchmark 지원.
- 랜덤 payload 범위와 사전 생성 payload pool 지원.
- 연결 수, warmup 응답 수, 측정 응답 수, 구간별 elapsed, payload 범위를 보여주는 Debug Profile 지표.
- 재현 가능한 benchmark evidence를 위한 CSV 출력.

### Release 서비스 모드

`FastPortServer`는 Debug 빌드에서는 console process로 실행되고, Release 빌드에서는 Windows 서비스로 실행됩니다.

서비스 이름:

```text
FastPortServerIOCP
```

---

## 빌드

개발자 PowerShell에서 실행:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  '.\FastPort.slnx' `
  '/t:FastPortServer;FastPortBenchmark' `
  '/p:Configuration=Release;Platform=x64' `
  /m /nologo /v:minimal
```

Debug 빌드:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  '.\FastPort.slnx' `
  '/t:FastPortServer;FastPortBenchmark' `
  '/p:Configuration=Debug;Platform=x64' `
  /m /nologo /v:minimal
```

빌드 결과 위치:

```text
_Builds\x64\Debug
_Builds\x64\Release
```

---

## IOCP 서버 실행

### Debug Console Mode

```powershell
.\_Builds\x64\Debug\FastPortServer.exe
```

### Release Service Mode

관리자 PowerShell에서 실행:

```powershell
.\_Builds\x64\Release\FastPortServer.exe install
sc.exe start FastPortServerIOCP
sc.exe query FastPortServerIOCP
```

서비스 중지/제거:

```powershell
sc.exe stop FastPortServerIOCP
.\_Builds\x64\Release\FastPortServer.exe uninstall
```

IOCP 서버는 `6628` 포트를 listen합니다.

---

## 현재 기준 Benchmark 실행

현재 최적화 기준 조건:

- Release 서버가 `FastPortServerIOCP` 서비스로 실행 중
- 실제 TCP 세션 1000개
- 4096-16384 byte 랜덤 binary payload
- 측정 request/response 100000회
- 세션당 warmup request/response 10회
- benchmark IO thread 8개

```powershell
.\_Builds\x64\Release\FastPortBenchmark.exe `
  --port 6628 `
  --sessions 1000 `
  --payload-min 4096 `
  --payload-max 16384 `
  --iterations 100000 `
  --warmup 10 `
  --io-threads 8 `
  --output docs\evidence\iocp_release_1000.csv
```

공식 benchmark 문서는 각 scenario를 10회 실행한 aggregate 기준으로 작성합니다. 1회 측정은 smoke check 또는 debugging 용도로만 사용합니다.

---

## 현재 기준 성능

문서: [docs/benchmark-results-05-iocp-release-1000-sessions.md](docs/benchmark-results-05-iocp-release-1000-sessions.md)

| 지표 | 값 |
|------|------:|
| 세션 | 1000 / 1000 connected |
| Payload | 4096-16384 bytes, avg 10346.37 bytes |
| Iterations | 100000 |
| Average RTT | 22.5596 ms |
| P50 RTT | 22.6236 ms |
| P99 RTT | 33.5659 ms |
| Throughput | 43526.33 packets/sec |
| Payload throughput | 429.48 MB/sec |
| Connection losses | 0 |

이후 최적화 결과는 이 workload를 기준선으로 삼아 `benchmark-results-06-*`, `benchmark-results-07-*` 순서로 기록합니다.

---

## 문서

- [빌드 가이드](docs/BUILD_GUIDE.md)
- [벤치마크 가이드](docs/BENCHMARK_GUIDE.md)
- [IOCP 아키텍처](docs/ARCHITECTURE_IOCP.md)
- [RIO 아키텍처](docs/ARCHITECTURE_RIO.md)
- [패킷 프로토콜](docs/PACKET_PROTOCOL.md)
- [프로젝트 구조](docs/PROJECT_STRUCTURE.md)

---

## Roadmap

- [x] IOCP 서비스 모드 benchmark 기준선.
- [x] 랜덤 payload pool 기반 다중 세션 benchmark.
- [ ] 10회 반복 aggregate 기준선 확정.
- [ ] 세션별 configurable in-flight depth.
- [ ] Zero-copy receive path.
- [ ] hot packet/session path object pool.
- [ ] graceful shutdown 및 keep-alive API 정리.

---

## License

MIT License
