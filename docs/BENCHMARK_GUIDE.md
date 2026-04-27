# Benchmark Guide

[English](BENCHMARK_GUIDE.md) | [한국어](ko/BENCHMARK_GUIDE.md)

This guide explains how to run `FastPortBenchmark` against the IOCP server.

---

## Overview

`FastPortBenchmark` measures request/response round-trip performance for FastPort servers.
The current optimization baseline targets the IOCP Release Windows service workload:

- 1000 real TCP sessions
- 4096-16384 byte random binary payload
- pre-generated payload pool
- 100000 measured request/response cycles
- 10 warmup request/response cycles per session
- 8 benchmark IO threads

Current baseline document:

[benchmark-results-05-iocp-release-1000-sessions.md](benchmark-results-05-iocp-release-1000-sessions.md)

---

## Metrics

| Metric | Description | Unit |
|--------|-------------|------|
| Latency (RTT) | Round-trip time for request-response | us, ms |
| Throughput | Packets/bytes completed per second | packets/sec, MB/s |
| P50/P90/P95/P99 | Percentile latency | us |
| Std Dev | Latency standard deviation | us |
| Connected sessions | Number of successfully connected TCP sessions | count |
| Warmup responses | Responses completed before measurement | count |
| Measured responses | Responses counted in the benchmark result | count |

---

## Build

Release:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  '.\FastPort.slnx' `
  '/t:FastPortServer;FastPortBenchmark' `
  '/p:Configuration=Release;Platform=x64' `
  /m /nologo /v:minimal
```

---

## Run the Release IOCP Server

Release `FastPortServer.exe` runs as a Windows service. Use an elevated PowerShell.

```powershell
.\_Builds\x64\Release\FastPortServer.exe install
sc.exe start FastPortServerIOCP
sc.exe query FastPortServerIOCP
```

Confirm that port `6628` is listening:

```powershell
netstat -ano | Select-String ':6628'
```

Stop and remove:

```powershell
sc.exe stop FastPortServerIOCP
.\_Builds\x64\Release\FastPortServer.exe uninstall
```

---

## Command Line Options

| Option | Description | Default |
|--------|-------------|---------|
| `--host <ip>` | Server address | `127.0.0.1` |
| `--port <port>` | Server port | `9000` |
| `--mode <mode>` | Network mode: `iocp` or `rio` | `iocp` |
| `--iterations <n>` | Total measured responses | `10000` |
| `--warmup <n>` | Warmup requests per session | `100` |
| `--payload <bytes>` | Fixed payload size | `64` |
| `--payload-min <bytes>` | Random payload minimum size | - |
| `--payload-max <bytes>` | Random payload maximum size | - |
| `--payload-pool <n>` | Number of pre-generated payloads | `1024` |
| `--sessions <n>` | Concurrent TCP sessions | `1` |
| `--io-threads <n>` | Benchmark IO worker threads | `2` |
| `--output <file>` | CSV output file | - |
| `--verbose` | Verbose output | `false` |
| `--pause-on-exit` | Wait for key before exit in Debug builds | `false` |
| `--help` | Show help | - |

---

## Current Baseline Command

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

In multi-session mode, `iterations` is the total measured response count across all sessions.
`warmup` is applied per session. For example, `--sessions 1000 --warmup 10` sends 10000 warmup request/response cycles.

---

## Official Run Policy

Use separate policies for quick checks and official benchmark documents.

| Purpose | Runs | Usage |
|---------|-----:|-------|
| Smoke check | 1 | Confirm the benchmark still completes |
| Official benchmark | 10 | Document performance and compare optimizations |

For official results, report best, worst, average, median, and standard deviation for:

- Average RTT
- P50 RTT
- P99 RTT
- Packets/sec
- MB/sec

If any run has connection loss or incomplete responses, record it as a failed run. Do not silently discard it.

---

## CSV Output

Timestamps are automatically appended to output filenames.

```text
results.csv -> results_2026-04-27-11-02-36.4966556.csv
```

### CSV Columns

| Column | Description |
|--------|-------------|
| `test_name` | Test name |
| `iterations` | Total measured responses |
| `payload_size` | Fixed payload size or maximum payload size |
| `avg_latency_ns` | Average latency |
| `min_latency_ns` | Minimum latency |
| `max_latency_ns` | Maximum latency |
| `p50_latency_ns` | P50 latency |
| `p90_latency_ns` | P90 latency |
| `p95_latency_ns` | P95 latency |
| `p99_latency_ns` | P99 latency |
| `stddev_ns` | Standard deviation |
| `packets_per_sec` | Packets per second |
| `mb_per_sec` | MB per second |
| `requested_sessions` | Requested session count |
| `connected_sessions` | Successfully connected sessions |
| `connection_losses` | Lost sessions during the run |
| `warmup_requests` / `warmup_responses` | Warmup request/response counts |
| `measured_requests` / `measured_responses` | Measured request/response counts |
| `payload_min_bytes` / `payload_max_bytes` | Random payload range |
| `payload_pool_size` | Number of pre-generated payloads |
| `connect_elapsed_ns` | Session connection elapsed time |
| `warmup_elapsed_ns` | Warmup elapsed time |
| `measured_elapsed_ns` | Measured elapsed time |

---

## Protocol

| Packet ID | Message | Direction |
|-----------|---------|-----------|
| `0x1001` | BenchmarkRequest | Client to server |
| `0x1002` | BenchmarkResponse | Server to client |

`BenchmarkResponse` echoes the payload and carries the original client timestamp.

---

## Notes

- The baseline benchmark is RTT-based and expects one response per request.
- It is not a pure send enqueue or maximum in-flight throughput test.
- The payload pool is generated before the measured section to reduce benchmark-side allocation noise.
- Use the same server build, client build, command-line arguments, and machine state for comparison runs.

---

## Related Documents

- [Current Benchmark Baseline](benchmark-results-05-iocp-release-1000-sessions.md)
- [IOCP Architecture](ARCHITECTURE_IOCP.md)
- [RIO Architecture](ARCHITECTURE_RIO.md)
- [Packet Protocol](PACKET_PROTOCOL.md)
- [Build Guide](BUILD_GUIDE.md)
