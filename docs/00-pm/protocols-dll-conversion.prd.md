# protocols-dll-conversion - Product Requirements Document

> **Date**: 2026-04-24
> **Author**: AnYounggun
> **Method**: bkit PM Analysis (lightweight, internal engineering refactor — PM Agent Team 생략)
> **Status**: Draft
> **Related**:
> - Plan: [protocols-dll-conversion.plan.md](../01-plan/features/protocols-dll-conversion.plan.md)
> - Design: [protocols-dll-conversion.design.md](../02-design/features/protocols-dll-conversion.design.md)

---

## Executive Summary

| Perspective | Content |
|-------------|---------|
| **Problem** | 동일한 `.proto` descriptor 를 가진 여러 static library (`LibNetworks`, `LibNetworksTests`, `FastPortBenchmark`, `FastPortServer` 등) 가 하나의 프로세스 공간에 동시 로드되면 Protobuf 의 전역 `DescriptorPool` 에 **"File already exists in database"** 치명적 충돌이 발생한다. 특히 `vstest.console.exe` 가 여러 test DLL 을 한 번에 로드할 때 reproducible crash 로 나타나 테스트 자동화를 막는다. |
| **Solution** | `Protocols` 프로젝트를 **Dynamic Library (DLL)** 로 전환. descriptor 가 프로세스당 1회만 등록되도록 단일 인스턴스 보장. MSVC `/FI` (Forced Include) 로 `PROTOCOLS_API` export 매크로를 Protobuf 생성 코드에 자동 주입. |
| **Target User** | FastPort 를 개발·유지보수하는 내부 엔지니어 (본인 포함). 특히 `vstest.console` 통합 실행, 단일 프로세스 내 여러 binary 로드가 필요한 테스트/벤치마크 시나리오를 운영하는 사람. |
| **Core Value** | **Build/Test 안정성 + 단일 descriptor 인스턴스로 인한 RAM 절감**. 여러 test DLL 병렬 실행 가능 → CI 시간 단축. 프로토콜 변경 시 DLL 교체만으로 링크된 모든 consumer 동시 갱신 (부차 효과). |

---

## 1. Opportunity Discovery

### 1.1 Current Behavior (As-Is)

각 consumer 프로젝트 (8개 `.vcxproj`) 가 `Protocols.vcxproj` 를 ProjectReference 로 참조하며, **static library** 로 링크한다. 결과:

| Binary | 포함되는 `Protocols` 코드 |
|---|---|
| `FastPortServer.exe` | 전량 복사본 (Commons/Tests/Admin/Benchmark `*.pb.cc`) |
| `FastPortServerRIO.exe` | 별개의 전량 복사본 |
| `FastPortClient.exe` | 별개의 전량 복사본 |
| `FastPortTestClient.exe` | 별개의 전량 복사본 |
| `FastPortBenchmark.exe` | 별개의 전량 복사본 |
| `LibNetworks.lib` (→ 위 exe 들에 재귀 포함) | - |
| `LibNetworksTests.dll` | 별개의 전량 복사본 |
| `LibNetworksRIOTests.dll` | 별개의 전량 복사본 |

각 binary 는 **단독 실행 시 정상**이지만, `vstest.console` 이 `LibNetworksTests.dll` + `LibNetworksRIOTests.dll` 을 같은 프로세스에서 로드하면:

```
[FATAL] CHECK failed: file != nullptr: File already exists in database:
  Commons.proto / Tests.proto / Admin.proto / Benchmark.proto
```

Protobuf global descriptor pool 은 프로세스당 단일 싱글톤이므로, 동일 descriptor 의 두 번째 등록 시도가 abort 를 유발.

### 1.2 Problem Framing

**왜 지금 중요한가**:
- `iosession-lifetime-race` feature 의 tests scope 진행 중 `LibNetworksTests` (IOCP) 와 `LibNetworksRIOTests` (RIO) 를 같은 `vstest.console` 호출로 묶어 regression 확보하고 싶음 → 현재는 별도 프로세스로 분리 실행 중 (우회책).
- 단독 실행도 static lib 6개 × proto 4개 = 약 **24개 descriptor 복사본** 이 RAM 에 상주. 의존 트리가 커질수록 낭비 증가.
- 새 `.proto` 추가 시 모든 consumer 가 재빌드 필요 (현재). DLL 전환 시 Protocols.dll 만 rebuild → link 시간 단축.

### 1.3 Top Ideas (Brainstorm, 자체 평가)

| 대안 | 검토 결과 |
|---|---|
| **(A) 전역 Pool 대신 proto `-use_unknown_fields` + 수동 등록 회피** | Protobuf 내부 singleton 설계를 거스름. 공식 지원 아님. 유지보수 악화. **기각**. |
| **(B) test DLL 별 프로세스 분리 (현재 우회책)** | CI 셋업 복잡, 병렬성 저하. 근본 해결 아님. **기각**. |
| **(C) `Protocols` 를 header-only 로 전환** | Protobuf 생성 코드는 `.pb.cc` 가 필수. 불가. **기각**. |
| **(D) `Protocols` DLL 전환** | Windows DLL loader 가 descriptor 초기화를 1회로 보장. 정석 해법. **채택**. |

→ Design 에서 DLL 전환의 3가지 구현 방식 (Option A Forced Include / Option B 수동 Preprocessor / Option C 수동 Header) 중 **Option A (Forced Include)** 선정.

---

## 2. Scope & Stakeholders

### 2.1 In-Scope
- `Protocols/Protocols.vcxproj` : `StaticLibrary` → `DynamicLibrary`
- `Protocols/ProtocolsAPI.h` 신규 — `PROTOCOLS_API` dllexport/dllimport 매크로
- `protoc` custom build command 에 `--cpp_out=dllexport_decl=PROTOCOLS_API:` 옵션 추가
- Forced Include (`/FI"ProtocolsAPI.h"`) 를 **Protocols 및 8개 의존 프로젝트** 전체에 적용
- 빌드 산출물 `Protocols.dll` + `Protocols.lib` (import lib) 가 통합 OutDir (`_Builds/x64/{Config}/`) 에 배치
- `grpc_cpp_plugin` 로 생성되는 gRPC 스텁 코드도 동일 export 매크로 영향 하 동작 검증

### 2.2 Out-of-Scope
- 기존 `.proto` 스키마 변경
- gRPC 전송 프로토콜 자체 변경
- Protobuf/gRPC 버전 업그레이드
- vcpkg 의존성 manifest 변경 (단, `MD/MDd` CRT 일관성 유지 검증)
- 외부 배포 빌드 시스템 (CI/CD 스크립트 수정은 후속 과제)

### 2.3 Stakeholders

| 역할 | 관심사 |
|---|---|
| 개발자 (본인) | 빌드 성공, 단위/통합 테스트 동시 실행 가능 |
| 테스트 Runner | `vstest.console` 한 번에 모든 테스트 DLL 로드 가능 |
| 서버 운영 (향후) | 배포 시 `Protocols.dll` 이 exe 와 같은 경로에 존재해야 함 (현재 통합 OutDir 으로 자동 해결) |

---

## 3. Success Criteria

측정 가능한 종결 기준 (Plan `§5 Success Criteria` 확장):

| # | 기준 | 검증 방법 |
|---|---|---|
| SC-1 | `Protocols.dll` + `Protocols.lib` 가 `_Builds/x64/{Debug,Release}/` 양쪽에 생성 | 빌드 후 파일 존재 확인 |
| SC-2 | 8개 의존 프로젝트 전부 Debug/Release × x64 빌드 성공 | MSBuild solution 빌드 |
| SC-3 | `vstest.console.exe LibNetworksTests.dll LibNetworksRIOTests.dll /Platform:x64` 단일 호출로 **crash 없이** 모든 테스트 실행 | 실제 실행 + exit code 0 |
| SC-4 | 기존 테스트 regression 0 (LibCommonsTests 37/37, LibNetworksTests 49/49) | 개별 및 통합 실행 비교 |
| SC-5 | `Protocols.dll` 이 누락된 상태에서 exe 실행 시 명확한 "missing DLL" 오류 (undefined behavior 아님) | 수동 재현 |
| SC-6 | 빌드 시간이 base 대비 ±10% 이내 유지 (DLL 링크 오버헤드 vs static link 감소 trade-off) | 측정 (optional) |

---

## 4. Risks & Mitigation

| Risk | Severity | Mitigation |
|---|:---:|---|
| Protobuf 생성 코드에 `dllexport_decl` 적용 시 일부 내부 심볼이 export 누락 → link error | 🟠 High | Option A Forced Include 로 **모든** `.pb.h` 에 일관된 매크로 주입. link 에러 시 `/NODEFAULTLIB` 아니라 누락 심볼을 명시 export |
| Consumer 프로젝트 중 1개라도 `/FI"ProtocolsAPI.h"` 누락 시 **descriptor 매크로 불일치 → silent ABI 불일치** | 🔴 Critical | `Commons.props` 에 공통 `/FI` 추가 고려 (FR-02 보강). 의존 프로젝트 목록 체크리스트화 |
| `Protocols.dll` runtime 경로 누락 시 exe startup 실패 | 🟡 Medium | 통합 OutDir 으로 이미 같은 경로. 배포 문서화 필요 |
| vcpkg `protobuf` / `grpc` 가 static linkage 기반 → DLL 전환 시 interop 문제 | 🟠 High | 현재 vcpkg triplet 이 `x64-windows` (MD = DLL runtime). 추가 대응 불요지만 link 시점 검증 |
| 여러 DLL 이 서로 다른 Protobuf CRT 를 쓰는 mixed-CRT 상황 | 🟡 Medium | `Commons.props` 이 `MD/MDd` 로 고정됨을 재확인 |
| CTO/운영 시각에서 **Protocols.dll 교체 시 링크된 모든 exe 가 바뀐 버전을 공유** → 테스트 없이 DLL 만 hotswap 하면 버전 드리프트 가능 | 🟢 Low | 배포 규약에 "Protocols.dll 교체 시 모든 exe 동기 배포" 명시 |

**ABI 호환성 주의**: 모든 의존 프로젝트가 동일 MSVC 버전 + 동일 CRT 로 컴파일되어야 함. 이는 `Commons.props:14` (`<PlatformToolset>`, `<RuntimeLibrary>MultiThreadedDebugDLL</...>`) 로 이미 강제.

---

## 5. Timeline & Effort

**Plan §6 의 3 마일스톤 유지** (예상 1 세션, 2-4 시간):

| Milestone | 작업 | 예상 시간 |
|---|---|---|
| M1 Core Conversion | `Protocols.vcxproj` 설정 변경 + `protoc` 옵션 + `ProtocolsAPI.h` | 30 min |
| M2 Macro Integration | 8개 의존 프로젝트에 `/FI` 추가 + `Commons.props` 공통화 검토 | 60 min |
| M3 Validation | Debug/Release 양쪽 빌드 + `vstest.console` 통합 실행 + regression 비교 | 60 min |

---

## 6. Decisions Locked

- **DLL export 방식**: Forced Include (`/FI`) — Design Option A 선택됨. Protobuf 생성 코드 수정 불요.
- **Export 매크로명**: `PROTOCOLS_API` — 기존 `PROTOCOLS_EXPORTS` 정의 대비 통일된 네이밍.
- **대상 프로젝트**: 해당 `.proto` descriptor 중복 문제가 있는 모든 consumer (8개). 빠짐 없는 적용 필수.
- **CRT**: 기존 `MD/MDd` 유지 (vcpkg 호환).
- **PM Agent Team skip 사유**: 내부 엔지니어링 refactor 로 시장/고객 분석이 적용되지 않음. 경량 PRD 형식 채택.

---

## 7. Out-of-Scope / Future Work

- **gRPC stub DLL 분리**: 현재는 `Protocols` DLL 에 gRPC 코드도 포함. 더 세분화할 경우 `Protocols.Proto.dll` / `Protocols.Grpc.dll` 으로 분할 가능. v2 후속.
- **Protobuf Arena allocator 적용**: 성능 최적화. 본 PRD 와 직교.
- **Protobuf Reflection 사용 최소화 + `LITE_RUNTIME` 전환**: 바이너리 크기/startup 시간 최적화. 별도 trade-off 분석 필요.

---

## 8. Acceptance

- [ ] SC-1 ~ SC-5 모두 통과
- [ ] Plan §5 Success Criteria 체크리스트 완료
- [ ] Design §4 구현 세부사항 전체 적용 확인
- [ ] `docs/03-analysis/protocols-dll-conversion.analysis.md` Gap Analysis Match Rate ≥ 90%
- [ ] 최소 1회 `vstest.console` 통합 실행 로그 증거 첨부

---

## 9. References

- Plan: `docs/01-plan/features/protocols-dll-conversion.plan.md`
- Design: `docs/02-design/features/protocols-dll-conversion.design.md`
- 현재 우회책 증거: 빌드 로그 `Protocols/vcxproj` 의 `x64-windows\x64-windows` 경로 오타 workaround (별건)
- 관련 인용: Google Protobuf 공식 Q&A "Preventing multiple registrations of the same proto" — DLL 분리가 공식 권장 해법
