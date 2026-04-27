# protocols-dll-conversion Completion Report

> **Feature**: protocols-dll-conversion
> **Date**: 2026-04-25
> **Author**: AnYounggun
> **Final Match Rate**: 91.67%
> **Validation Coverage**: SC-1 ~ SC-5 passed, SC-6 optional not measured
> **Status**: Reported

---

## Executive Summary

| Perspective | Content |
|-------------|---------|
| **Problem** | `Protocols` 가 static library 로 각 consumer 에 복사되어, `vstest.console` 이 여러 test DLL 을 단일 프로세스에 로드할 때 Protobuf 전역 `DescriptorPool` 에 동일 descriptor 가 중복 등록되어 fatal crash 발생 |
| **Solution** | `Protocols` 프로젝트를 Dynamic Library 로 전환하고, `PROTOCOLS_API` export/import 매크로를 `/FI"ProtocolsAPI.h"` Forced Include 로 전체 consumer 에 중앙 주입 |
| **Function/UX Effect** | `Protocols.dll` 을 프로세스당 1회 로드하여 descriptor singleton 을 보장. IOCP/RIO test DLL 통합 실행 시 descriptor 중복 등록 crash 제거 |
| **Core Value** | 테스트 안정성 확보, 통합 `vstest.console` 실행 가능, Protocol 변경 시 DLL 중심 재배포 구조 확립 |

### Value Delivered

| Metric | Target | Delivered | 평가 |
|---|---|---|:-:|
| Mandatory success criteria | SC-1 ~ SC-5 | **5/5 pass** | OK |
| Optional success criteria | SC-6 build time +/-10% | Not measured | Deferred |
| Formal match rate | >= 90% | **91.67%** | OK |
| Debug artifacts | `Protocols.dll` + `Protocols.lib` | Present in `_Builds/x64/Debug/` | OK |
| Release artifacts | `Protocols.dll` + `Protocols.lib` | Present in `_Builds/x64/Release/` | OK |
| Regression tests | Existing tests green | Debug + Release integrated `vstest` 73/73, `LibCommonsTests` 37/37, `LibNetworksTests` 52/52 | OK |
| Missing DLL behavior | Clear loader failure | `STATUS_DLL_NOT_FOUND` confirmed | OK |

---

## Context Anchor

| Key | Value |
|-----|-------|
| **WHY** | Protobuf descriptor 를 프로세스당 단일 인스턴스로 보장하여 static link 중복 등록 fatal crash 를 근본 제거 |
| **WHO** | FastPort 내부 엔지니어, 테스트/벤치마크 실행자, 단일 프로세스에서 여러 binary 를 함께 로드하는 개발 흐름 |
| **RISK** | `/FI` 누락, export symbol 누락, Runtime DLL 누락, mixed CRT |
| **SUCCESS** | `Protocols.dll` + import lib 생성, 8 consumer 빌드, 통합 test DLL 실행, regression 0, missing DLL 오류 명확화 |
| **SCOPE** | `Protocols.vcxproj`, `ProtocolsAPI.h`, `Commons.props`, 문서 업데이트, Debug/Release x64 검증 |

---

## Journey 요약

### PRD

- 문제 정의: static link 된 Protobuf descriptor 복사본이 같은 프로세스에서 중복 등록되어 abort 발생
- 채택 방향: `Protocols` 를 DLL 로 분리하여 Windows loader 가 descriptor 초기화를 1회로 제한
- 성공 기준: SC-1 ~ SC-6 정의, SC-6 은 optional metric

### Plan

- M1: `ProtocolsAPI.h` 생성, `Protocols.vcxproj` DynamicLibrary 전환, `protoc` `dllexport_decl` 적용
- M2: `Commons.props` 에 Forced Include 중앙화, 8 consumer 빌드
- M3: Debug/Release 산출물, 통합 `vstest.console`, regression, missing DLL 동작 검증

### Design

- Option A Forced Include 선택
- `PROTOCOLS_EXPORTS` 는 `Protocols.vcxproj` 에서만 정의
- consumer 는 동일 header 를 강제 include 하지만 `PROTOCOLS_EXPORTS` 가 없으므로 자동 `dllimport` 경로 사용
- Protobuf 생성 코드는 `--cpp_out=dllexport_decl=PROTOCOLS_API:` 로 export annotation 을 받음

### Do

1. `Protocols/ProtocolsAPI.h` 추가
2. `Protocols/Protocols.vcxproj` 를 `DynamicLibrary` 로 전환
3. Debug/Release 및 Win32/x64 configuration 에 `_USRDLL;PROTOCOLS_EXPORTS` 정의 반영
4. x64 Debug/Release `protoc` custom build command 에 `dllexport_decl=PROTOCOLS_API` 적용
5. `Commons.props` 에 `/FI"$(SolutionDir)Protocols\ProtocolsAPI.h"` 추가
6. `docs/BUILD_GUIDE.md`, `docs/PROJECT_STRUCTURE.md` 에 DLL 산출물과 배포 규약 반영

### Check

`.bkit/state/pdca-status.json` 의 `do_validation_green` 기록 기준:

- M1 + M2 + M3 validation green
- SC-1 ~ SC-5 all pass
- Debug + Release integrated `vstest` 73/73 pass
- `LibCommonsTests` 37/37 pass
- `LibNetworksTests` 52/52 pass
- `Protocols.dll` 누락 시 `STATUS_DLL_NOT_FOUND` 확인

Formal analysis 는 `docs/03-analysis/protocols-dll-conversion.analysis.md` 에 기록했다. Match Rate 는 **91.67%** 이며, 주요 residual gap 은 `*.grpc.pb.h` 에 `PROTOCOLS_API` 가 직접 주입되지 않는 점이다. 현재 bkit validation 은 green 이므로 즉시 blocking 은 아니지만, gRPC stub 을 public DLL API 로 사용할 경우 archive 전에 확인하는 편이 안전하다.

---

## Key Decisions & Outcomes

| 결정 | 준수 | 결과 |
|---|:-:|---|
| `Protocols` 를 DLL 로 전환 | OK | `_Builds/x64/{Debug,Release}/Protocols.dll` 생성 |
| Option A Forced Include 사용 | OK | `Commons.props` 에 `/FI"$(SolutionDir)Protocols\ProtocolsAPI.h"` 중앙 적용 |
| `PROTOCOLS_EXPORTS` 는 DLL 빌드 측에만 정의 | OK | `Protocols.vcxproj` 에 Debug/Release 정의 반영 |
| Protobuf generated code 에 `PROTOCOLS_API` annotation 적용 | OK | x64 Debug/Release custom build command 에 `dllexport_decl=PROTOCOLS_API` 적용 |
| Runtime DLL 누락을 silent failure 로 두지 않음 | OK | `STATUS_DLL_NOT_FOUND` 확인 |
| 배포 규약 문서화 | OK | `BUILD_GUIDE.md`, `PROJECT_STRUCTURE.md` 반영 |

---

## Success Criteria Final Status

| ID | 기준 | 상태 | 근거 |
|----|------|:---:|---|
| SC-1 | `Protocols.dll` + `Protocols.lib` 가 Debug/Release 양쪽에 생성 | OK | `_Builds/x64/Debug`, `_Builds/x64/Release` 에 산출물 존재 |
| SC-2 | 8개 consumer 프로젝트 Debug/Release x64 빌드 성공 | OK | bkit validation 기록: M1+M2+M3 validation green |
| SC-3 | `vstest.console` 단일 호출로 test DLL 통합 실행, crash 0 | OK | bkit validation 기록: integrated `vstest` 73/73 pass |
| SC-4 | 기존 테스트 regression 0 | OK | `LibCommonsTests` 37/37, `LibNetworksTests` 52/52 pass |
| SC-5 | `Protocols.dll` 누락 시 명확한 loader 오류 | OK | `STATUS_DLL_NOT_FOUND` 확인 |
| SC-6 | build time baseline 대비 +/-10% | Deferred | optional metric, 이번 보고서에서는 미측정 |

Mandatory success criteria: **5/5 pass**

---

## 구현 통계

| 항목 | 값 |
|---|---|
| 신규 파일 | `Protocols/ProtocolsAPI.h`, `docs/04-report/protocols-dll-conversion.report.md` |
| 수정 파일 | `Commons.props`, `Protocols/Protocols.vcxproj`, `docs/BUILD_GUIDE.md`, `docs/PROJECT_STRUCTURE.md` |
| 핵심 build 변경 | `StaticLibrary` -> `DynamicLibrary`, `_USRDLL`, `PROTOCOLS_EXPORTS`, `dllexport_decl=PROTOCOLS_API` |
| 핵심 integration 변경 | `Commons.props` Forced Include 로 consumer 전체에 `PROTOCOLS_API` 주입 |
| 검증 범위 | Debug + Release x64, integrated `vstest`, LibCommonsTests, LibNetworksTests, missing DLL |

---

## Key Learnings

### 1. Generated code 의 export 매크로는 Forced Include 가 가장 유지보수성이 높다

Protobuf 생성 파일을 직접 수정하지 않고도 `PROTOCOLS_API` 를 주입할 수 있었다. 새 `.proto` 가 추가되어도 동일 custom build command 와 `Commons.props` 규칙을 따르면 export/import 경로가 유지된다.

### 2. Build-system refactor 는 consumer 누락 리스크가 가장 크다

8개 consumer 중 하나라도 다른 macro path 를 타면 link error 또는 ABI 불일치가 생길 수 있다. 이번 전환은 `Commons.props` 중앙화로 per-project 누락 가능성을 낮췄다.

### 3. DLL 전환은 배포 규약까지 같이 닫아야 완료다

`Protocols.dll` 이 exe 와 같은 OutDir 에 있어야 실행된다. 문서에 DLL 동반 배포와 DLL 단독 hotswap 금지 규칙을 남긴 것은 운영 리스크를 줄이는 데 필요하다.

---

## 남은 작업

### 즉시 권장

- `dumpbin /imports` 결과를 evidence 문서로 남겨 8 consumer 의 `Protocols.dll` import 를 명시
- gRPC stub 을 public DLL API 로 사용할 계획이 있으면 `*.grpc.pb.h` 에 `PROTOCOLS_API` annotation 을 주입할 수 있는지 확인

### 후속 스코프

- CI/CD 스크립트가 있다면 `Protocols.dll` 동반 배포 확인 추가
- SC-6 build time 측정: DLL 전환 전/후 Debug/Release x64 빌드 시간 비교
- 새 `.proto` 추가 워크플로우를 `CLAUDE.md` 또는 별도 개발 가이드에 반영

---

## 문서 링크

- PRD: [docs/00-pm/protocols-dll-conversion.prd.md](../../../00-pm/protocols-dll-conversion.prd.md)
- Plan: [protocols-dll-conversion.plan.md](./protocols-dll-conversion.plan.md)
- Design: [protocols-dll-conversion.design.md](./protocols-dll-conversion.design.md)
- Analysis: [protocols-dll-conversion.analysis.md](./protocols-dll-conversion.analysis.md)
- Report: [docs/04-report/protocols-dll-conversion.report.md](./protocols-dll-conversion.report.md)

---

## Version History

| Version | Date | Changes | Author |
|---------|------|---------|--------|
| 0.1 | 2026-04-25 | Initial completion report based on bkit validation green record | AnYounggun |
| 0.2 | 2026-04-25 | Formal analysis result reflected: Match Rate 91.67%, Analysis link added | AnYounggun |
