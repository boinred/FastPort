# FastPort

[English](./README.md) | [한국어](./README.ko.md)

**High-performance asynchronous networking framework for Windows IOCP and RIO**

FastPort is a C++20 modules-based networking project focused on IOCP server performance, benchmark reproducibility, and future RIO support. The current optimization baseline is the IOCP Release Windows service workload: 1000 real TCP sessions with 4K-16K random binary payloads.

---

## Project Overview

| Item | Description |
|------|-------------|
| Goal | Build and optimize a high-performance Windows networking framework |
| Primary engine | IOCP |
| Secondary engine | RIO, maintained in separate RIO projects |
| Language | C++20 modules (`.ixx`) |
| Platform | Windows x64 |
| Toolchain | Visual Studio / MSBuild |

---

## Tech Stack

| Category | Technology |
|----------|------------|
| Async I/O | Windows IOCP, AcceptEx, ConnectEx |
| RIO | Registered I/O projects under `LibNetworksRIO` and `FastPortServerRIO` |
| Serialization | Protocol Buffers |
| Logging | `LibCommons::Logger` wrapper over spdlog |
| CLI | cxxopts |
| Package management | vcpkg |

---

## Key Implementations

### IOCP Engine

- Asynchronous accept/connect using `AcceptEx` and `ConnectEx`.
- Worker thread processing through IOCP completion ports.
- Zero-byte receive support for idle session efficiency.
- Sequential one-outstanding-send behavior per session.
- Scatter/gather send path with `WSABUF`.
- Session idle checking and server status/admin support.

### Benchmarking

- Real multi-session benchmark support through `FastPortBenchmark`.
- Random payload ranges with pre-generated payload pools.
- Debug profile fields for connection count, warmup responses, measured responses, elapsed times, and payload range.
- CSV output for reproducible benchmark evidence.

### Release Service Mode

`FastPortServer` runs as a console process in Debug builds and as a Windows service in Release builds. The Release service name is:

```text
FastPortServerIOCP
```

---

## Build

From a developer PowerShell:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  '.\FastPort.slnx' `
  '/t:FastPortServer;FastPortBenchmark' `
  '/p:Configuration=Release;Platform=x64' `
  /m /nologo /v:minimal
```

Debug build:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  '.\FastPort.slnx' `
  '/t:FastPortServer;FastPortBenchmark' `
  '/p:Configuration=Debug;Platform=x64' `
  /m /nologo /v:minimal
```

Build outputs are written under:

```text
_Builds\x64\Debug
_Builds\x64\Release
```

---

## Run the IOCP Server

### Debug Console Mode

```powershell
.\_Builds\x64\Debug\FastPortServer.exe
```

### Release Service Mode

Run from an elevated PowerShell:

```powershell
.\_Builds\x64\Release\FastPortServer.exe install
sc.exe start FastPortServerIOCP
sc.exe query FastPortServerIOCP
```

Stop or remove the service:

```powershell
sc.exe stop FastPortServerIOCP
.\_Builds\x64\Release\FastPortServer.exe uninstall
```

The IOCP server listens on port `6628`.

---

## Run the Current Baseline Benchmark

The current optimization baseline is:

- Release server running as `FastPortServerIOCP`
- 1000 real TCP sessions
- 4096-16384 byte random binary payload
- 100000 measured request/response cycles
- 10 warmup request/response cycles per session
- 8 benchmark IO threads

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

Official benchmark documents should use 10 runs per scenario. Single-run measurements are only for smoke checks or debugging.

---

## Current Baseline Result

Document: [docs/benchmark-results-05-iocp-release-1000-sessions.md](docs/benchmark-results-05-iocp-release-1000-sessions.md)

| Metric | Value |
|--------|------:|
| Sessions | 1000 / 1000 connected |
| Payload | 4096-16384 bytes, avg 10346.37 bytes |
| Iterations | 100000 |
| Average RTT | 22.5596 ms |
| P50 RTT | 22.6236 ms |
| P99 RTT | 33.5659 ms |
| Throughput | 43526.33 packets/sec |
| Payload throughput | 429.48 MB/sec |
| Connection losses | 0 |

Future optimization results should continue as `benchmark-results-06-*`, `benchmark-results-07-*`, and so on, using this workload as the baseline.

---

## Documentation

- [Build Guide](docs/BUILD_GUIDE.md)
- [Benchmark Guide](docs/BENCHMARK_GUIDE.md)
- [IOCP Architecture](docs/ARCHITECTURE_IOCP.md)
- [RIO Architecture](docs/ARCHITECTURE_RIO.md)
- [Packet Protocol](docs/PACKET_PROTOCOL.md)
- [Project Structure](docs/PROJECT_STRUCTURE.md)

---

## Roadmap

- [x] IOCP service-mode benchmark baseline.
- [x] Multi-session benchmark with random payload pool.
- [ ] 10-run aggregate benchmark baseline.
- [ ] Configurable in-flight depth per session.
- [ ] Zero-copy receive path.
- [ ] Object pool integration for hot packet/session paths.
- [ ] Graceful shutdown and keep-alive API hardening.

---

## License

MIT License
