# iosession-lifetime-race Verify Log

> **Purpose**: 패치 전/후 10k conn × 5분 stress 실측 결과 기록.
> Design §8.5 Stress Reproducer Scenario 의 S1~S7 단계 체크리스트.
> Plan Q4: Release 빌드 재현 선행.

- **Feature**: iosession-lifetime-race
- **Log owner**: AnYounggun
- **Created**: 2026-04-21

---

## 0. Build Status (공통)

- [x] FastPortServer.exe (IOCP) Debug|x64 — `_Builds/x64/Debug/FastPortServer.exe`
- [x] FastPortServer.exe Release|x64 — `_Builds/x64/Release/FastPortServer.exe`
- [x] FastPortServerRIO.exe Debug|x64 — `_Builds/x64/Debug/FastPortServerRIO.exe`
- [x] FastPortServerRIO.exe Release|x64 — `_Builds/x64/Release/FastPortServerRIO.exe`
- [x] FastPortTestClient.exe Debug|x64 — `_Builds/x64/Debug/FastPortTestClient.exe`
- [x] FastPortTestClient.exe Release|x64 — `_Builds/x64/Release/FastPortTestClient.exe`

---

## 1. 실행 Runbook

### 1.1 공통 준비

1. **TCP port 확인**: `netstat -an | findstr 6628` → 다른 프로세스 점유 없어야 함.
   - 점유 중이면 port 변경 (TestClient Stress 탭 `Target Port` 수정).
2. **Ephemeral port 여유**: 10k conn + 100 churn/s × 5분 = 30k churn → 10k outstanding + TIME_WAIT 수만개.
   - 필요 시 레지스트리 `HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters` 의 `MaxUserPort` (기본 5000, 최대 65535) 조정. TIME_WAIT 재사용 활성화 권장 (`TcpTimedWaitDelay`=30).
3. **Handle limit**: Windows 기본 프로세스당 10k+ socket OK. `_setmaxstdio` 무관.
4. **Debug CRT heap check**: Debug 빌드는 기본 활성. `_CrtSetDbgFlag(_CRTDBG_LEAK_CHECK_DF)` 옵션은 서버 `main` 에 추가해 볼 것.

### 1.2 Pre-patch 재현 단계

IOCP Debug 기준. RIO / Release 반복 동일.

1. **Server 실행** (새 콘솔)
   - `_Builds/x64/Debug/FastPortServer.exe`
   - 로그 경로: `_Builds/x64/Debug/loggers/` (일반) 또는 stdout
   - 크래시 감지 용도로 Visual Studio 디버거 attach 권장 (`F5` 로 띄우면 자동)
2. **TestClient 실행** (별도 콘솔/세션)
   - `_Builds/x64/Debug/FastPortTestClient.exe`
   - `Stress` 탭 이동
   - Target IP: `127.0.0.1`, Port: `6628`
   - Target Conns: `10000`, Churn rate: `100`, Duration: `300`
   - Server mode: IOCP (UI 라벨 — 실제 서버는 별도 프로세스)
   - `[Start Stress]` 클릭
3. **관찰 포인트**
   - Build-up 구간 (약 10s 내외): Connect Attempts 10000, Connect Failures 0 (이상적). 실패 있으면 port exhaustion 의심.
   - Churn 구간: Active ≈ 10000 유지, Churned 가 초당 ~100 증가.
   - **크래시 기대**: Churn 구간 1~5분 내 Server 프로세스 crash → 디버거가 break, 콜스택/레지스터 기록.
4. **크래시 발생 시 수집**
   - Visual Studio Exception 창의 **Call Stack** 전체 복사
   - `this` pointer 값 (예: `0xdddddddd...` 이면 UAF 확정)
   - `m_RecvOverlapped`/`m_SendOverlapped` 의 WSABufs size/capacity
   - 크래시 시점의 Server 로그 꼬리 ~100줄
5. **크래시 미발생 시** (5분 완주)
   - Churn 을 더 올려보기 (200/s, 500/s). Duration 도 늘려 10분 시도.
   - 여전히 재현 안 되면: 기록 후 "under stress 조건에서 재현 불가 — 확률적 race" 로 logbook 에 기록. 다음 단계로 진행 가능.

### 1.3 Release 빌드 반복

동일 절차를 `_Builds/x64/Release/` 바이너리로. Release 는 최적화로 race window 가 달라질 수 있음.

### 1.4 (선택) RIO 동등성

`FastPortServerRIO.exe` 로 동일 stress. Design §3.4 / §4.3 에 따라 RIO 도 동일 취약성 예상.

---

## 2. Pre-patch Results

### 2.1 IOCP — Debug|x64

| Field | Value |
|---|---|
| Run date | 2026-04-21 |
| Server binary | `_Builds/x64/Debug/FastPortServer.exe` |
| Client binary | `_Builds/x64/Debug/FastPortTestClient.exe` (echo 활성 버전) |
| Params | 10000 conns / 100 churn/s / 300 s / echo on / payload 4 B (TBD: 최종값 확정) |
| 크래시 위치 | `LibNetworks/iOService.cpp:86` — `pConsumer->OnIOCompleted(...)` virtual call |
| `pConsumer` (=this) 값 | **`0x000002845ac6dca0`** (valid-looking heap 주소, CRT fill 아님) |
| 해석 | **Heap re-use 타입 UAF**: 세션이 freed → 같은 chunk 가 다른 객체로 재할당 → worker 가 dequeue 해서 vtable 호출 → 가짜 vtable 타고 AV. `0xdddd...` fill 이 덮이기 전에 OS heap 이 즉시 재사용한 경우. |
| Call stack 상단 | `IOService worker → line 86 pConsumer->OnIOCompleted` |
| 결론 | **UAF 재현 ✅** — Design §2.2 의 "worker 가 raw this 로 freed 세션 dispatch" race 가 정확히 확정. Self-retain 패치가 필요/충분 조건. |

**주요 의미**: 원래 보고됐던 `IOSession.cpp:169` 의 `WSABufs.push_back` 크래시는 같은 UAF 가 한 단계 더 안쪽에서 터진 것. Echo 트래픽으로 completion 빈도가 오르면서 race 가 더 일찍 (OnIOCompleted 진입 시점) 터짐. 두 지점 모두 동일 근본 원인 → self-retain 으로 둘 다 해결 예상.

### 2.2 IOCP — Release|x64

| Field | Value |
|---|---|
| Run date | _(pending)_ |
| Server binary | `_Builds/x64/Release/FastPortServer.exe` |
| Client binary | `_Builds/x64/Release/FastPortTestClient.exe` |
| Params | 10000 conns / 100 churn/s / 300 s |
| Build-up 실패 수 | _(pending)_ |
| Churn 완료 수 | _(pending)_ |
| 크래시 발생 시각 | _(pending)_ |
| 크래시 위치 (디스어셈블리 주소 + symbol) | _(pending)_ |
| Dump 경로 | _(pending — WER 크래시 dump 또는 VS attach)_ |
| 결론 | _(pending)_ |

### 2.3 (선택) RIO — Debug|x64

| Field | Value |
|---|---|
| Run date | _(pending)_ |
| Server binary | `_Builds/x64/Debug/FastPortServerRIO.exe` |
| Params | 10000 conns / 100 churn/s / 300 s |
| 결과 | _(pending)_ |

### 2.4 (선택) RIO — Release|x64

| Field | Value |
|---|---|
| Run date | _(pending)_ |
| Server binary | `_Builds/x64/Release/FastPortServerRIO.exe` |
| 결과 | _(pending)_ |

---

## 3. Post-patch Results (이후 `--scope iocp/rio` 완료 후 채움)

### 3.1 IOCP — Debug|x64

| Field | Expected | Actual |
|---|---|---|
| UAF crash | **0** | _(pending)_ |
| Build-up 실패 수 | 0 | _(pending)_ |
| Churn 완료 수 | ≈ 30,000 | _(pending)_ |
| Accept 로그 수 vs ~Session 로그 수 | 일치 | _(pending)_ |
| CRT leak report | 0 | _(pending)_ |
| Throughput diff vs pre-patch | ≤ 3% 감소 | _(pending)_ |

### 3.2 IOCP — Release|x64

(동일 포맷)

### 3.3 RIO — Debug|x64

### 3.4 RIO — Release|x64

---

## 4. Environment

| Item | Value |
|---|---|
| OS | Windows 11 Pro (10.0.26200) |
| Toolchain | MSVC 14.50.35717, Platform Toolset v145 |
| Project | FastPort (main branch) |
| Git HEAD at pre-patch run | `985f42f` (docs(iosession-lifetime-race): Plan + Design) |
| Git HEAD at post-patch run | _(fill after --scope iocp + rio 적용)_ |

---

## 5. Notes / Diff Observations

이 섹션은 실행 중 발견된 부가 사항 기록 (예: "Release 가 Debug 보다 재현 빈도 낮음", "RIO 는 ChurnRate 200/s 에서만 재현" 등).

- _(pending)_

---

## 6. Decision

- [ ] Pre-patch 재현 확인 → `--scope iocp` 진행 가능
- [ ] Pre-patch 재현 **실패** → confidence 낮지만 `--scope iocp` 진행 후 post-patch 에서 leak/stat 으로 간접 검증
- [ ] (다른 크래시 발견 시) 별도 이슈 처리 후 본 PDCA 재개

---

## Version History

| Version | Date | Changes | Author |
|---|---|---|---|
| 0.1 | 2026-04-21 | 템플릿 작성. Pre-patch 실행 대기 중. | AnYounggun |
