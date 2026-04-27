# Benchmark Results Log

## Phase: IOCP Release Service Baseline - 1000 Sessions

This phase establishes the new optimization baseline for `FastPortServer`.
Older benchmark logs used a different workload shape and are no longer used as optimization baselines.

The baseline workload creates 1000 real TCP sessions, uses a pre-generated binary payload pool in the 4K-16K range, and measures request/response round-trip latency through `FastPortBenchmark`.
The server runs in Release Windows service mode as `FastPortServerIOCP`.

### Consolidated Results

| Run Timestamp | Test Name | Iterations | Payload Size | Avg Latency (ms) | P50 (ms) | P99 (ms) | Packets/s | MB/s |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| 2026-04-27 11:02:36 | LatencyTest | 100000 | 4096-16384 random | 22.5596 | 22.6236 | 33.5659 | 43526.33 | 429.48 |

CSV evidence:

`docs/evidence/iocp_release_1000_20260427_2026-04-27-11-02-36.4966556.csv`

Detailed run profile:

| Metric | Value |
| :--- | :--- |
| Service name | `FastPortServerIOCP` |
| Listen endpoint | `0.0.0.0:6628` |
| Connected sessions | 1000 / 1000 |
| Connection losses | 0 |
| Connect elapsed | 639.88 ms |
| Warmup responses | 10000 / 10000 |
| Warmup elapsed | 124.53 ms |
| Measured responses | 100000 / 100000 |
| Measured elapsed | 2297.46 ms |
| Payload average | 10346.37 bytes |
| Payload pool size | 1024 |

---

### 📊 Baseline Analysis

This result is the reference point for future IOCP optimization work.

#### 1. Workload Definition
*   **Sessions**: 1000 real TCP sessions.
*   **Payload**: 4096-16384 bytes random binary payload.
*   **Iteration count**: 100000 measured request/response cycles.
*   **Server mode**: Release Windows service mode.
*   **Benchmark shape**: RTT-based request/response, not pure send enqueue flood.

#### 2. Current Throughput
*   **Packets/s**: **43,526.33 PPS**.
*   **MB/s**: **429.48 MB/s**.
*   **Insight**: This baseline measures real payload movement under multi-session pressure, so future improvements should be judged against both PPS and MB/s.

#### 3. Current Latency
*   **Average RTT**: **22.5596ms**.
*   **P50 RTT**: **22.6236ms**.
*   **P99 RTT**: **33.5659ms**.
*   **Insight**: The RTT includes session scheduling, packet framing, echo response dispatch, send/recv completion, and benchmark-side response waiting.

### 💡 Conclusion
The **IOCP Release Service Baseline** is now the primary benchmark reference for future optimization.

*   **Baseline throughput**: ~43.5K PPS / ~429.5 MB/s.
*   **Baseline latency**: ~22.6ms average RTT / ~33.6ms P99 RTT.
*   **Stability**: 1000 sessions connected, 0 connection loss, 100000/100000 measured responses.
*   **Measurement caveat**: This is an RTT benchmark with one response expected per request. It is not yet a maximum in-flight throughput test.

**Next Steps:**
Future optimization results should be documented as `benchmark-results-06-*`, `benchmark-results-07-*`, and so on, using this workload as the comparison baseline.

---

### 🧪 Benchmark Run Policy

Official benchmark documents should use **10 runs per scenario**.
Single-run measurements are allowed only for smoke checks or debugging.

Recommended policy:

*   **Smoke test**: 1 run, used only to confirm the benchmark still completes.
*   **Official benchmark**: 10 runs with the same server build, client build, command-line arguments, and machine state.
*   **Reported values**: include best, worst, average, median, and standard deviation for Avg RTT, P50, P99, PPS, and MB/s.
*   **Failure handling**: if any run has connection loss or incomplete responses, record it and do not silently drop it.
*   **Environment note**: record server mode, build configuration, session count, payload range, warmup count, iteration count, IO thread count, and CSV evidence paths.

The current row above is the first Release service baseline run.
The next official optimization comparison should run 10 samples and compare aggregate values against this baseline.

---

### 🇰🇷 벤치마크 결과 상세 분석 (Korean Analysis)

이번 문서부터 FastPort IOCP 최적화의 기준선을 **Release 서비스 모드 + 1000 실제 세션 + 4K-16K 랜덤 payload**로 변경합니다.
이전 01-04 benchmark 문서는 workload가 달라 직접 비교 기준으로 사용하지 않습니다.

#### 1. 기준 workload
*   1000개의 실제 TCP 세션을 연결합니다.
*   4096-16384 bytes 범위의 binary payload pool을 미리 생성합니다.
*   warmup은 세션당 10회, 총 10000회입니다.
*   측정 구간은 총 100000 request/response입니다.
*   서버는 Release Windows 서비스 모드입니다.

#### 2. 현재 기준 성능
*   **Average RTT**: **22.5596ms**
*   **P50 RTT**: **22.6236ms**
*   **P99 RTT**: **33.5659ms**
*   **Packets/s**: **43,526.33 PPS**
*   **MB/s**: **429.48 MB/s**

이 값은 앞으로의 최적화 결과와 비교할 기준선입니다.
단순 micro benchmark가 아니라 실제 다중 세션 echo workload에 가깝기 때문에, PPS와 MB/s, P99를 함께 봐야 합니다.

#### 3. 10회 반복 측정 정책
앞으로 공식 benchmark 문서는 각 조건을 **10회 반복 실행**한 뒤 작성하는 것이 좋습니다.
한 번만 돌린 결과는 OS 스케줄링, 서비스 상태, CPU boost, 백그라운드 작업의 영향을 크게 받을 수 있습니다.

권장 집계 방식:

*   평균값만 보지 말고 median과 worst run을 함께 기록합니다.
*   P99는 run별 변동이 크므로 10회 중 best/worst도 같이 봅니다.
*   connection loss 또는 response 누락이 있으면 실패 run으로 기록합니다.
*   smoke check는 1회로 충분하지만, 문서화할 성능 수치는 10회 기준으로 남깁니다.

#### 4. 🏁 향후 최적화 방향
다음 최적화부터는 05번을 기준으로 `benchmark-results-06-*` 형식으로 기록합니다.

다음 병목 후보:

*   세션별 configurable in-flight depth 추가
*   `PacketFramer`와 response dispatch 비용 분석
*   send/recv path의 allocation 여부 재점검
*   종료 시점 disconnect 로그량 축소

📊 **기준 성능 요약**

| 항목 | **IOCP Release Service Baseline (05)** |
| :--- | :--- |
| **세션 수** | 1000 |
| **Payload** | 4096-16384 random, avg 10346.37 bytes |
| **Iterations** | 100000 |
| **Average RTT** | 22.5596ms |
| **P50 RTT** | 22.6236ms |
| **P99 RTT** | 33.5659ms |
| **처리량** (PPS) | 43,526.33 |
| **처리량** (MB/s) | 429.48 |
| **검증 의미** | 서비스 모드 다중 세션 기준선 |
