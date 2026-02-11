# Benchmark Guide

[English](BENCHMARK_GUIDE.md) | [한국어](ko/BENCHMARK_GUIDE.md)

This guide explains how to use the FastPort network performance benchmark tool.

---

## 📊 Overview

`FastPortBenchmark` is a tool for measuring the performance of IOCP/RIO-based network servers.

### Metrics

| Metric | Description | Unit |
|--------|-------------|------|
| **Latency (RTT)** | Round-trip time for request-response | µs, ms |
| **Throughput** | Packets/Bytes processed per second | packets/sec, MB/s |
| **P50/P90/P95/P99** | Percentile Latency | µs |
| **Std Dev** | Standard Deviation (Jitter) | µs |

---

## 🚀 Usage

### Basic Execution

```powershell
# 1. Start the server first
.\FastPortServer.exe

# 2. Run the benchmark (in a separate terminal)
.\FastPortBenchmark.exe
```

### Command Line Options

| Option | Description | Default |
|--------|-------------|---------|
| `--host <ip>` | Server address | 127.0.0.1 |
| `--port <port>` | Server port | 9000 |
| `--mode <mode>` | Network mode: `iocp` or `rio` | `iocp` |
| `--iterations <n>` | Number of iterations | 10000 |
| `--warmup <n>` | Number of warmup iterations | 100 |
| `--payload <bytes>` | Payload size in bytes | 64 |
| `--output <file>` | Output CSV file path | - |
| `--verbose` | Enable verbose output | false |
| `--help` | Show help message | - |

### Examples

```powershell
# Default test (IOCP mode)
FastPortBenchmark.exe

# RIO mode test (High Performance)
FastPortBenchmark.exe --mode rio

# Specify iterations and payload size
FastPortBenchmark.exe --iterations 1000 --payload 256

# Remote server test + Save to CSV
FastPortBenchmark.exe --host 192.168.1.100 --port 9001 --output results.csv

# Verbose mode
FastPortBenchmark.exe --iterations 100 --verbose
```

---

## 📁 Output Files

### CSV Filename Format

Timestamps are automatically appended to output filenames.

```
results.csv → results_2024-01-15-14-30.csv
benchmark.csv → benchmark_2024-01-15-14-30.csv
```

### CSV Columns

| Column | Description |
|--------|-------------|
| `test_name` | Name of the test |
| `iterations` | Number of iterations |
| `payload_size` | Payload size (bytes) |
| `avg_latency_ns` | Average latency (ns) |
| `min_latency_ns` | Minimum latency (ns) |
| `max_latency_ns` | Maximum latency (ns) |
| `p50_latency_ns` | 50th percentile latency (ns) |
| `p90_latency_ns` | 90th percentile latency (ns) |
| `p95_latency_ns` | 95th percentile latency (ns) |
| `p99_latency_ns` | 99th percentile latency (ns) |
| `stddev_ns` | Standard deviation (ns) |
| `packets_per_sec` | Packets per second |
| `mb_per_sec` | Megabytes per second |

---

## 📋 Output Example

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

## 📈 Benchmark Scenarios

### 1. Basic Latency Test

```powershell
FastPortBenchmark.exe --iterations 10000 --payload 64
```

Measures basic RTT for small packets.

### 2. Large Packet Test

```powershell
FastPortBenchmark.exe --iterations 1000 --payload 4096
```

Measures performance for handling large packets.

### 3. Long-Running Stability Test

```powershell
FastPortBenchmark.exe --iterations 100000 --warmup 1000
```

Measures performance changes over a long period.

### 4. Comparison Test (IOCP vs RIO)

```powershell
# Measure IOCP
FastPortBenchmark.exe --mode iocp --output iocp_result.csv

# Measure RIO
FastPortBenchmark.exe --mode rio --output rio_result.csv

# Compare results (using Excel or scripts)
```

---

## 🔧 Protocol Specification

### Packet IDs

| Packet ID | Message | Direction |
|-----------|---------|-----------|
| `0x1001` | BenchmarkRequest | Client → Server |
| `0x1002` | BenchmarkResponse | Server → Client |

### BenchmarkRequest

```protobuf
message BenchmarkRequest {
    Header header = 1;
    uint64 client_timestamp_ns = 2;  // Client send timestamp (ns)
    uint32 sequence = 3;              // Sequence number
    bytes payload = 4;                // Variable size payload
}
```

### BenchmarkResponse

```protobuf
message BenchmarkResponse {
    Header header = 1;
    ResultCode result = 2;
    uint64 client_timestamp_ns = 3;      // Client original timestamp
    uint64 server_recv_timestamp_ns = 4; // Server receive timestamp
    uint64 server_send_timestamp_ns = 5; // Server send timestamp
    uint32 sequence = 6;                 // Sequence number
    bytes payload = 7;                   // Payload (Echo)
}
```

---

## ⚠️ Notes

### Warmup

- Warmup is required for initial connection setup and JIT optimization.
- Default is 100; 1000+ recommended for precise measurement.

### Network Environment

- Use localhost to minimize network delay for pure code performance testing.
- Consider network bandwidth/latency for remote tests.

### System Load

- Minimize CPU/network usage of other processes.
- Recommended to average results from multiple runs.

### Interpreting Results

- If P99 latency is significantly higher than average, check for tail latency issues.
- High Std Dev indicates unstable performance.

---

## 🔗 Related Documents

- [IOCP Architecture](ARCHITECTURE_IOCP.md)
- [RIO Architecture](ARCHITECTURE_RIO.md)
- [Packet Protocol](PACKET_PROTOCOL.md)
- [Build Guide](BUILD_GUIDE.md)