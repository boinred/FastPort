# protocols-dll-conversion Gap Analysis

> **Feature**: protocols-dll-conversion
> **Date**: 2026-04-25
> **Author**: AnYounggun
> **Phase**: Check / Analyze
> **Match Rate**: 91.67%
> **Decision**: Match rate >= 90%, report/archive 가능

---

## Executive Summary

`Protocols` DLL 전환은 설계의 핵심 목표와 대부분 일치한다. `Protocols.vcxproj` 는 `DynamicLibrary` 로 전환되었고, `ProtocolsAPI.h` + `PROTOCOLS_EXPORTS` + `Commons.props` Forced Include 조합으로 Protobuf generated message code 에 `PROTOCOLS_API` 가 주입되었다. Debug/Release x64 산출물과 bkit validation 기록도 SC-1 ~ SC-5 통과를 뒷받침한다.

주요 gap 은 `*.grpc.pb.h` 파일에 `PROTOCOLS_API` 가 직접 주입되지 않는 점이다. 현재 bkit validation 에서는 통합 테스트가 green 이므로 즉시 blocking 은 아니지만, gRPC stub class/function symbol 을 외부 consumer 가 직접 사용할 경우 export 범위를 별도 확인해야 한다.

---

## Source Documents

| Type | Path |
|---|---|
| PRD | `docs/00-pm/protocols-dll-conversion.prd.md` |
| Plan | `docs/01-plan/features/protocols-dll-conversion.plan.md` |
| Design | `docs/02-design/features/protocols-dll-conversion.design.md` |
| Report | `docs/04-report/protocols-dll-conversion.report.md` |

---

## Implementation Scan

| Area | Evidence | Result |
|---|---|:-:|
| DLL project type | `Protocols/Protocols.vcxproj` has `<ConfigurationType>DynamicLibrary</ConfigurationType>` for all configurations | Match |
| Export macro header | `Protocols/ProtocolsAPI.h` defines `PROTOCOLS_API` as `dllexport` when `PROTOCOLS_EXPORTS` is set, otherwise `dllimport` | Match |
| DLL build macro | `Protocols/Protocols.vcxproj` defines `_USRDLL;PROTOCOLS_EXPORTS` | Match |
| Protobuf cpp export option | x64 Debug/Release custom build uses `--cpp_out=dllexport_decl=PROTOCOLS_API:$(ProjectDir)` | Match |
| Forced Include | `Commons.props` adds `/FI"$(SolutionDir)Protocols\ProtocolsAPI.h"` | Match |
| Consumer propagation | 8 listed consumer projects import `Commons.props` and reference `Protocols.vcxproj` | Match |
| Generated message headers | `Protocols/*.pb.h` contains `PROTOCOLS_API` annotations | Match |
| Generated gRPC headers | `Protocols/*.grpc.pb.h` does not contain `PROTOCOLS_API` annotations | Partial |
| Build artifacts | `_Builds/x64/Debug` and `_Builds/x64/Release` contain `Protocols.dll` and `Protocols.lib` | Match |
| Documentation | `docs/BUILD_GUIDE.md` and `docs/PROJECT_STRUCTURE.md` describe Protocols DLL and deployment rule | Match |
| Validation record | `.bkit/state/pdca-status.json` records SC-1..SC-5 pass, Debug+Release integrated `vstest` 73/73 pass | Match |

---

## Design Item Comparison

| ID | Design / Plan Item | Implementation | Category | Score |
|----|--------------------|----------------|----------|:---:|
| D-01 | `Protocols` 를 Dynamic Library 로 전환 | `Protocols.vcxproj` 의 configuration type 이 `DynamicLibrary` 로 변경됨 | Match | 1 |
| D-02 | `ProtocolsAPI.h` 생성 | `Protocols/ProtocolsAPI.h` 신규 생성, `PROTOCOLS_API` 정의 포함 | Match | 1 |
| D-03 | `PROTOCOLS_EXPORTS` 로 export/import 분기 | `Protocols.vcxproj` 에 `PROTOCOLS_EXPORTS` 정의, consumer 는 미정의 상태로 `dllimport` 경로 | Match | 1 |
| D-04 | `protoc` command 에 `dllexport_decl=PROTOCOLS_API` 적용 | x64 Debug/Release custom build command 에 적용됨 | Match | 1 |
| D-05 | `/FI"ProtocolsAPI.h"` 적용 | `Commons.props` 에 Forced Include 중앙 적용. x64 `Protocols` 및 consumers 가 `Commons.props` import | Match | 1 |
| D-06 | 모든 dependent project 에 Forced Include 전파 | 8 consumer 가 `Commons.props` 를 import하고 `Protocols.vcxproj` 를 reference | Match | 1 |
| D-07 | `Protocols` include path 확보 | `Commons.props` 의 `PublicIncludeDirectories=$(SolutionDir)` 로 `Protocols/ProtocolsAPI.h` 경로 접근 가능 | Match | 1 |
| D-08 | Debug/Release x64 `Protocols.dll` + `Protocols.lib` 생성 | `_Builds/x64/{Debug,Release}/` 산출물 확인 | Match | 1 |
| D-09 | `*.pb.h` generated code 에 export macro 주입 | `Admin.pb.h`, `Benchmark.pb.h`, `Commons.pb.h`, `Tests.pb.h` 에 `PROTOCOLS_API` 존재 | Match | 1 |
| D-10 | gRPC stub 도 동일 export 범위 적용 | `.grpc.pb.h` 에 직접 `PROTOCOLS_API` 없음. 현재 validation 은 통과했으나 gRPC stub 외부 symbol export 는 추가 확인 필요 | Changed / Partial | 0 |
| D-11 | 통합 테스트에서 descriptor 중복 등록 crash 제거 | bkit validation: Debug+Release integrated `vstest` 73/73 pass | Match | 1 |
| D-12 | 배포/구조 문서 업데이트 | `BUILD_GUIDE.md`, `PROJECT_STRUCTURE.md` 업데이트 완료 | Match | 1 |

**Match Rate**: `11 / 12 * 100 = 91.67%`

---

## Success Criteria Check

| ID | Criteria | Result | Evidence |
|----|----------|:---:|---|
| SC-1 | `Protocols.dll` + `Protocols.lib` Debug/Release 생성 | Pass | `_Builds/x64/Debug`, `_Builds/x64/Release` 산출물 존재 |
| SC-2 | 8 consumer Debug/Release x64 빌드 성공 | Pass | `.bkit/state/pdca-status.json` 의 `do_validation_green` 기록 |
| SC-3 | 통합 `vstest.console` 실행 crash 0 | Pass | Debug+Release integrated `vstest` 73/73 pass 기록 |
| SC-4 | 기존 테스트 regression 0 | Pass | `LibCommonsTests` 37/37, `LibNetworksTests` 52/52 pass 기록 |
| SC-5 | `Protocols.dll` 누락 시 명확한 loader 오류 | Pass | `STATUS_DLL_NOT_FOUND` 확인 기록 |
| SC-6 | build time baseline 대비 +/-10% | Not Measured | optional metric, evidence 없음 |

---

## Gaps

### G-01: gRPC generated headers lack direct `PROTOCOLS_API`

| Field | Content |
|---|---|
| Category | Changed / Partial |
| Severity | Medium |
| Evidence | `Protocols/*.grpc.pb.h` 에 `PROTOCOLS_API` 검색 결과 없음 |
| Impact | 현재 검증은 통과했으나, 외부 consumer 가 gRPC stub class/function 을 DLL boundary 너머 직접 사용할 경우 symbol export 누락 가능성 확인 필요 |
| Suggested Action | 필요 시 `--grpc_out=dllexport_decl=PROTOCOLS_API:$(ProjectDir)` 지원 여부를 확인하고, generated `.grpc.pb.h` 에 annotation 이 들어가는지 재생성 검증 |

### G-02: formal import evidence document is not present

| Field | Content |
|---|---|
| Category | Missing Evidence |
| Severity | Low |
| Evidence | `dumpbin /imports` 결과가 별도 evidence 문서로 남아 있지 않음 |
| Impact | bkit 상태 기록만으로는 code review 시 binary import 상태를 줄 단위로 추적하기 어렵다 |
| Suggested Action | `docs/evidence/protocols-dll-conversion-imports.md` 에 8 consumer `Protocols.dll` import 결과 저장 |

---

## Risk Assessment

| Risk | Current State | Residual Risk |
|---|---|---|
| `/FI` 누락 | `Commons.props` 중앙화 + consumer import 확인 | Low |
| Protobuf message export 누락 | `.pb.h` 에 `PROTOCOLS_API` 존재 | Low |
| gRPC stub export 누락 | `.grpc.pb.h` 에 direct annotation 없음 | Medium |
| Runtime DLL 누락 | `STATUS_DLL_NOT_FOUND` 확인, 배포 문서화 | Low |
| Mixed CRT | `Commons.props` 기준 유지, 별도 변경 없음 | Low |
| Build time regression | 미측정 | Medium |

---

## Decision

Match Rate 는 **91.67%** 로 report 기준인 90% 이상이다. 현재 상태에서는 `$pdca report protocols-dll-conversion` 또는 `$pdca archive protocols-dll-conversion` 로 진행 가능하다.

단, gRPC stub 을 public DLL API 로 사용할 계획이 있으면 archive 전에 G-01 을 확인하는 편이 안전하다.

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-04-25 | Initial gap analysis for protocols DLL conversion | AnYounggun |
