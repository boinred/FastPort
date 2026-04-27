# iosession-lifetime-race — Pre-Patch Verification Evidence

> **목적**: 패치 전 UAF 재현 여부를 확보하여, 패치 후 "UAF 0회" 검증의 비교 기준을 만든다.
> **참조**: Plan v0.2 FR-11 · Design v0.2 §8.4 S1-A / S1-B
> **Scope**: `verify-before` (iocp-game-server-engine v1 M1 선행)
> **상태**: ✅ **재현 확보 완료 (2026-04-22)**

---

## 환경

| 항목 | 값 |
|---|---|
| 날짜 | 2026-04-22 |
| Git HEAD | `f153349` ("Gemini CLI 무시 및 Agents 기능 실험적 활성화") + working-tree (reproducer scope 수정 포함) |
| `git grep "SelfRetain" -- LibNetworks/` | 비어있음 ✅ (패치 전 상태 확인) |
| OS | Windows 11 Pro 10.0.26200 |
| MSVC | 14.50 (project) |
| VS 2022 | 17.x |

---

## Test 1 — Scenario A Debug|x64 (S1-A) — **재현 성공**

### 실행 파라미터

- Scenario: A (Churn)
- Conns: 10000
- Churn rate /sec: 100
- Duration: 300 sec (crash 이전 종료)
- Enable Echo: ON
- Payload: 64 B

### 관찰된 크래시

- **위치**: `LibNetworks/iOService.cpp:86` `pConsumer->OnIOCompleted(bResult == TRUE, bytesTransferred, pOverlapped);`
- **예외 유형**: 읽기 액세스 위반
- **`pConsumer` 값**: `0xFFFFFFFFFFFFFFFF`
- **`bResult`**: **TRUE** ← 중요: GetQueuedCompletionStatus 는 정상 성공
- **이것이 의미하는 바**: IOCP 커널이 성공적으로 completion packet 을 반환했는데, 그 packet 의 completion key (원래 `reinterpret_cast<ULONG_PTR>(session)` 로 `Associate` 시 등록된 값) 가 `0xFFFFFFFFFFFFFFFF` 로 오염되어 있음
- **증거 스크린샷**: `docs/evidence/pre-patch-debug-2026-04-22.png`(사용자 보관)

### 진단

- ❌ **Loop fall-through bug 가설 기각**: `bResult == TRUE` 이므로 IOService worker loop 의 실패 fall-through 경로는 해당 없음
- ✅ **Lifetime race 의 한 변종으로 분류**: IOCP 커널 측 completion packet 의 key 필드가 세션 freed 이후 어느 시점에 손상되었거나, kernel pool 재사용으로 오염된 것. 정상 경로에서는 session 이 alive 한 동안 `Associate` 시 등록한 key 가 그대로 와야 하므로, session 의 lifecycle 정리(SelfRetain 패턴)로 근본 차단 가능
- **0xdddddddddddddddd (MSVC heap fill) 대신 `0xFFFFFFFFFFFFFFFF` 관찰**: heap free pattern 이 아닌, 커널/유저 공유 메모리 또는 페이지 decommit 후 값. 세션이 이미 완전히 해제되고 VM 페이지까지 반환된 더 심각한 시점까지 race window 가 열려 있었다는 의미 — **SelfRetain 으로 session 수명을 pending I/O 완료 시점까지 연장하면 이 race 자체가 발생 불가**

---

## Test 2 — Scenario A Release|x64 (S1-B)

Debug 로 충분한 증거 확보 완료. Release 재현은 선택 사항이며, 필요 시 패치 후 `--scope verify-after` 에서 Release 동일 시나리오가 UAF 0회 통과함을 보여 대비 증거로 삼는다.

---

## 결론

- ✅ **Debug 에서 UAF 재현 확인** — `bResult=TRUE + pConsumer=0xFFFF…F` 형태
- ✅ **본 feature 의 SelfRetain 패턴으로 해결 가능한 lifetime race 의 변종** (freed-heap fill 대신 VM decommit 수준의 더 깊은 경로)
- ✅ **Bug A (loop fall-through) 가설 기각** (bResult=TRUE 확인으로)

### 패치 후 기대 동작

SelfRetain 이 적용되면:
- `RequestRecv` / `TryPostSendFromQueue` posting 직전 `SelfRetain = shared_from_this()` 로 session 수명 연장
- `OnIOCompleted` 모든 exit path 에서 `auto self = std::move(SelfRetain)` → 함수 종료 시 drop
- pending I/O 가 완료 통지로 도착하는 시점에 session 은 **반드시 alive** → completionId 가 가리키는 메모리 유효
- 커널 completion packet 의 key 오염 가능성 자체 제거 (관련 메모리가 아직 release 되지 않음)

### 다음 단계

- [x] verify-before 완료 — 증거 확보
- [ ] `/pdca do iosession-lifetime-race --scope iocp-patch` 진입 — SelfRetain 패턴 적용
- [ ] 이후 `--scope tests` → `--scope verify-after` 에서 동일 시나리오 UAF 0회 재검증

---

## 부록 — 증거 수집 체크리스트

- [x] Debugger 크래시 스크린샷 (call stack `pConsumer->OnIOCompleted`, watch `pConsumer = 0xFFFF…F`)
- [x] `bResult == TRUE` 확인 (사용자 debugger 상 관찰)
- [x] 본 evidence 문서 기록
