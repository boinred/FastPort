# protocols-dll-conversion Planning Document

> **Summary**: `Protocols` 프로젝트를 Static Library 에서 Dynamic Library (DLL) 로 전환하여 Protobuf descriptor 중복 등록 문제 근본 해결.
>
> **Project**: FastPort
> **Version**: v0.2
> **Author**: AnYounggun
> **Date**: 2026-04-24
> **Status**: Draft
> **PRD**: [protocols-dll-conversion.prd.md](../../00-pm/protocols-dll-conversion.prd.md)
> **Design**: [protocols-dll-conversion.design.md](../../02-design/features/protocols-dll-conversion.design.md)

---

## Executive Summary

| Perspective | Content |
|-------------|---------|
| **Problem** | 동일 `.proto` descriptor 를 가진 여러 static library (`LibNetworks`, `LibNetworksTests`, `FastPortBenchmark` 등) 가 한 프로세스에 로드되면 Protobuf 전역 `DescriptorPool` 에 **"File already exists in database"** fatal crash 발생. 특히 `vstest.console` 이 IOCP/RIO test DLL 을 동시 로드할 때 reproducible. |
| **Solution** | `Protocols` 를 **Dynamic Library** 로 전환. MSVC `/FI"ProtocolsAPI.h"` Forced Include 로 `PROTOCOLS_API` export 매크로를 Protobuf 생성 코드에 자동 주입 (Design Option A 선택). `Commons.props` 중앙집중화. |
| **Function/UX Effect** | `vstest.console` 단일 호출로 IOCP/RIO test DLL 통합 실행 가능, 프로세스당 descriptor 복사본 1개로 축소 → 메모리 절감, 프로토콜 변경 시 DLL 만 재빌드 → link 시간 단축. |
| **Core Value** | Build/Test 안정성 + 단일 인스턴스 descriptor 로 RAM 절감 + 병렬 테스트 실행 → CI 시간 단축. Protocol 변경 시 DLL 교체만으로 모든 consumer 동시 갱신 (부차 효과). |

---

## Context Anchor

> Auto-generated from Executive Summary. Propagated to Design/Do documents for context continuity.

| Key | Value |
|-----|-------|
| **WHY** | Protobuf descriptor 프로세스 단일 인스턴스 보장으로 중복 등록 fatal crash 근본 해결 |
| **WHO** | FastPort 내부 엔지니어. `vstest.console` 통합 실행 + 단일 프로세스 내 여러 binary 동시 로드가 필요한 개발/테스트/벤치마크 운영자 |
| **RISK** | Critical: 8 consumer 중 1개라도 `/FI` 누락 시 silent ABI 불일치. High: Protobuf 생성 코드 symbol export 누락 link 에러 |
| **SUCCESS** | SC-3 `vstest.console` 단일 호출로 `LibNetworksTests.dll` + `LibNetworksRIOTests.dll` 동시 실행 시 crash 0건 + 기존 테스트 regression 0 |
| **SCOPE** | `Protocols.vcxproj` + 8개 consumer (`LibNetworks`, `LibNetworksTests`, `LibNetworksRIOTests`, `FastPortServer`, `FastPortServerRIO`, `FastPortClient`, `FastPortTestClient`, `FastPortBenchmark`) + `Commons.props` + `ProtocolsAPI.h` 신규 |

---

## 1. Overview

### 1.1 Purpose

Protobuf generated code (`*.pb.cc`, `*.grpc.pb.cc`) 가 포함된 `Protocols` 프로젝트를 Static Library 에서 Dynamic Library 로 전환한다. 목적은 Protobuf 의 전역 `DescriptorPool` 에 동일 descriptor 가 2회 이상 등록될 때 발생하는 fatal abort 를 프로세스당 DLL 1회 로드로 근본 차단하는 것이다.

### 1.2 Background

- `iosession-lifetime-race` feature 의 tests scope 진행 중, `LibNetworksTests` (IOCP) 와 `LibNetworksRIOTests` (RIO) 를 같은 `vstest.console` 호출로 묶어 regression 확보를 시도했으나 두 DLL 모두 static 으로 `Protocols` 를 흡수하고 있어 descriptor 충돌 발생.
- 현재는 두 DLL 을 별도 프로세스로 분리 실행하는 우회책 사용 중. CI 병렬성 저하, 셋업 복잡성 증가.
- 8개 consumer 프로젝트 × 4개 `.proto` = 약 **24개 descriptor 복사본** 이 프로세스별 상주 (RAM 낭비).
- Protobuf 공식 문서에서 권장하는 해법은 공유 descriptor 를 제공하는 빌드 아티팩트를 **하나의 DLL** 로 분리하는 것.

### 1.3 Related Documents

- PRD: `docs/00-pm/protocols-dll-conversion.prd.md`
- Design: `docs/02-design/features/protocols-dll-conversion.design.md` (Option A Forced Include 선정)
- Parent feature 맥락: `iosession-lifetime-race` tests scope — 이 feature 완료 후 통합 test run 가능

---

## 2. Scope

### 2.1 In Scope

- [ ] `Protocols/Protocols.vcxproj` — `ConfigurationType` `StaticLibrary` → `DynamicLibrary` 전환
- [ ] `Protocols/ProtocolsAPI.h` 신규 생성 — `PROTOCOLS_API` dllexport/dllimport 매크로 정의
- [ ] `protoc` custom build command 에 `--cpp_out=dllexport_decl=PROTOCOLS_API:` 옵션 추가
- [ ] `Commons.props` 에 공통 `/FI"$(SolutionDir)Protocols\ProtocolsAPI.h"` 적용 (중앙집중화 — 8개 consumer 일괄 반영)
- [ ] `Protocols.vcxproj` PreprocessorDefinitions 에 `PROTOCOLS_EXPORTS` 추가 (DLL 빌드 측 export 경로 트리거)
- [ ] Debug + Release × x64 양쪽 빌드 산출물 검증 (`_Builds/x64/Debug/`, `_Builds/x64/Release/`)
- [ ] gRPC 스텁 (`*.grpc.pb.cc/h`) 도 동일 export 범위 적용
- [ ] `docs/BUILD_GUIDE.md` + `docs/PROJECT_STRUCTURE.md` 에 DLL 전환 반영
- [ ] 단일 branch (`feat/protocols-dll-conversion`) 에서 작업 → 통합 테스트 통과 후 PR 단위 머지

### 2.2 Out of Scope

- `.proto` 스키마 변경 (신규 메시지 추가 / 필드 수정 등)
- gRPC 전송 프로토콜 자체 변경
- Protobuf / gRPC 라이브러리 버전 업그레이드
- vcpkg 의존성 manifest / triplet 변경 (단, 현재 `MD/MDd` 호환성 유지 검증은 포함)
- 외부 배포/CI 스크립트 변경 (후속 과제)
- **gRPC stub 만 별도 DLL 로 분리 (v2 후속)**
- **Protobuf Arena allocator 적용** (성능 최적화, 본 feature 와 직교)
- **Protobuf `LITE_RUNTIME` 전환** — 바이너리 크기/startup 최적화. Q2 edge case 발생 시에도 회피책 아닌 근본 전환이라 별도 scope

---

## 3. Requirements

### 3.1 Functional Requirements

| ID | Requirement | Priority | Status |
|----|-------------|:--------:|:------:|
| FR-01 | `Protocols.vcxproj` 를 `DynamicLibrary` 로 전환하여 `Protocols.dll` + `Protocols.lib` (import lib) 생성 | High | Pending |
| FR-02 | `ProtocolsAPI.h` 를 생성하고 `PROTOCOLS_API` 매크로를 `PROTOCOLS_EXPORTS` 정의 여부에 따라 `__declspec(dllexport/dllimport)` 로 분기 | High | Pending |
| FR-03 | `protoc` 커스텀 빌드 명령에 `--cpp_out=dllexport_decl=PROTOCOLS_API:$(ProjectDir)` 적용하여 Protobuf 생성 코드에 export 주석 삽입 | High | Pending |
| FR-04 | `Commons.props` 에 `/FI"ProtocolsAPI.h"` 공통 적용 (8 consumer 개별 설정 아닌 중앙집중) | High | Pending |
| FR-05 | 8개 consumer 프로젝트 전부 link 산출물에서 `Protocols.dll` 의 import lib 참조 확인 (MSBuild 자동) | High | Pending |
| FR-06 | Debug + Release × x64 양쪽 빌드가 warning-as-error 없이 green | High | Pending |
| FR-07 | `vstest.console` 단일 호출로 `LibNetworksTests.dll` + `LibNetworksRIOTests.dll` 동시 실행, crash 0건 | Critical | Pending |
| FR-08 | 기존 단위 테스트 regression 0 (LibCommonsTests 37/37, LibNetworksTests 49/49 유지) | High | Pending |
| FR-09 | `docs/BUILD_GUIDE.md`, `docs/PROJECT_STRUCTURE.md`, `docs/ARCHITECTURE_IOCP.md` 에 DLL 전환 반영 | Medium | Pending |

### 3.2 Non-Functional Requirements

| Category | Criteria | Measurement Method |
|----------|----------|--------------------|
| **Build Time** | 전환 전 대비 ±10% 이내 (DLL link 오버헤드 vs static 축소의 trade-off) | `msbuild /v:m` 총 시간 3회 평균 비교 |
| **Binary Size** | 총 프로세스 메모리 상주 descriptor 약 24개 → 1개 (Protocols.dll 내) 축소 | sysinternals `vmmap` 또는 RSS 비교 |
| **CRT Consistency** | 모든 프로젝트 `MultiThreadedDebugDLL`/`MultiThreadedDLL` (MD/MDd) 유지 | `Commons.props:14` 고정, per-project override 없음 확인 |
| **Runtime** | `Protocols.dll` 가 exe 와 동일 OutDir (`_Builds/x64/{Config}/`) 에 배치되어 자동 로드 | 빌드 후 파일 존재 확인 |
| **Robustness** | `Protocols.dll` 누락 시 Windows 기본 loader 오류 다이얼로그 (silent failure 금지) | 수동 재현 |

---

## 4. Success Criteria

### 4.1 Definition of Done

PRD 의 SC-1 ~ SC-6 전부 통과:

- [ ] **SC-1**: `Protocols.dll` + `Protocols.lib` 가 `_Builds/x64/Debug/` 및 `_Builds/x64/Release/` 양쪽에 생성
- [ ] **SC-2**: 8개 consumer 프로젝트 전부 Debug/Release × x64 빌드 성공 (warning-as-error 기준)
- [ ] **SC-3**: `vstest.console.exe LibNetworksTests.dll LibNetworksRIOTests.dll /Platform:x64` 단일 호출로 **crash 없이** 모든 테스트 실행 — **가장 중요한 최종 증빙**
- [ ] **SC-4**: 기존 테스트 regression 0 — LibCommonsTests 37/37, LibNetworksTests 49/49, LibNetworksRIOTests 기존 건수 유지
- [ ] **SC-5**: `Protocols.dll` 누락 상태에서 exe 실행 시 Windows loader 의 명확한 "DLL not found" 오류 (undefined behavior / silent crash 아님)
- [ ] **SC-6**: 빌드 시간이 baseline 대비 ±10% 이내 (optional metric)

### 4.2 Quality Criteria

- [ ] `Commons.props` 수정이 1곳에서 8 consumer 에 일관 적용 확인
- [ ] Protobuf `*.pb.h` 가 생성 후 `PROTOCOLS_API` 매크로 참조 확인 (diff 전/후 비교)
- [ ] `/FI` 누락 프로젝트 없음 체크리스트 완성
- [ ] Code review 시 staged 변경 파일 목록이 Impact Analysis §6 과 1:1 매칭

---

## 5. Risks and Mitigation

PRD §4 에서 상속, severity 태그 유지:

| # | Risk | Impact | Likelihood | Severity | Mitigation |
|---|------|:------:|:----------:|:--------:|------------|
| R-1 | 8개 consumer 중 1개라도 `/FI"ProtocolsAPI.h"` 누락 → silent ABI 불일치로 runtime 미스 매치 | High | Medium | 🔴 Critical | `Commons.props` 중앙집중 적용 (FR-04). 프로젝트별 `AdditionalOptions` 에서 `/FI` 중복 정의 방지. 빌드 후 `dumpbin /imports` 로 각 binary 의 `Protocols.dll` 참조 확인 |
| R-2 | Protobuf 생성 코드에 `dllexport_decl` 적용 시 일부 내부 심볼 (constexpr static, inline) export 누락 → link error | High | Medium | 🟠 High | Q2=c 실용 전략 — `/FI` 로 대부분 해결 기대. link 에러 발생 시 누락 심볼 명시 export 또는 후속 패치. 최악의 경우 해당 proto 만 static 유지 (`#ifdef` 격리) |
| R-3 | vcpkg `protobuf` / `grpc` 가 static 링크 기반 → DLL 전환 시 interop 문제 (특히 Arena, MessageLite 등) | Medium | Low | 🟠 High | 현재 vcpkg triplet `x64-windows` 은 MD runtime (DLL) 기반. Protobuf 런타임 자체도 이미 동적 — 추가 대응 불요 예상. Link 시점 확인 |
| R-4 | `Protocols.dll` runtime 경로 누락 시 exe startup 실패 | Medium | Low | 🟡 Medium | 통합 `OutDir` 으로 이미 exe 와 같은 경로. 배포 시 묶음 보장 문서화 (FR-09) |
| R-5 | 혼합 CRT (MD/MDd) 상태가 consumer 간 불일치 → heap 오염 / 이중 해제 | High | Low | 🟡 Medium | `Commons.props:14` 가 이미 강제. per-project override 감사. 빌드 로그에 CRT 불일치 경고 발생 시 즉시 중단 |
| R-6 | `Protocols.dll` 교체 시 링크된 모든 exe 가 바뀐 버전을 공유 → hotswap 시 버전 드리프트 | Low | Low | 🟢 Low | 배포 규약: `Protocols.dll` 교체 시 모든 exe 동기 배포 명시 (`docs/BUILD_GUIDE.md` 반영). 런타임 버전 체크는 out-of-scope |

---

## 6. Impact Analysis

### 6.1 Changed Resources

| Resource | Type | Change Description |
|----------|------|--------------------|
| `Protocols/Protocols.vcxproj` | Build config | StaticLibrary → DynamicLibrary, PreprocessorDefinitions 추가, `protoc` command 옵션 추가 |
| `Protocols/ProtocolsAPI.h` | Header (신규) | `PROTOCOLS_API` 매크로 정의 |
| `Commons.props` | MSBuild props | `AdditionalOptions` 에 `/FI"ProtocolsAPI.h"` 공통 추가 |
| `Protocols/*.pb.cc`, `*.pb.h` | Generated code | 다음 `msbuild` 시 `dllexport_decl` 주석 재생성 (source control 에는 생성물 제외) |
| `docs/BUILD_GUIDE.md` | Documentation | DLL 배포 / 경로 규약 섹션 추가 |
| `docs/PROJECT_STRUCTURE.md` | Documentation | Protocols 프로젝트 카드 업데이트 (static → dynamic) |

### 6.2 Current Consumers

8개 consumer 프로젝트 — 전부 `ProjectReference` 로 `Protocols.vcxproj` 참조 중. DLL 전환 후 자동으로 import lib 기반 링크로 전환됨 (MSBuild 의 `LinkLibraryDependencies` 자동 처리). 각 consumer 는 **컴파일 레벨에서** `/FI"ProtocolsAPI.h"` 를 통해 `PROTOCOLS_API` 매크로가 `dllimport` 로 확장되어야 함:

| Consumer Project | 직접 include 하는 헤더 | Impact |
|------------------|------------------------|--------|
| `LibNetworks` | `Commons.pb.h`, `Tests.pb.h` (IOSession::SendMessage 등) | 핵심. `/FI` 누락 시 link 에러 first surface |
| `FastPortServer` | `Admin.pb.h`, `Tests.pb.h` (IOCPInboundSession 핸들러) | link 에러 시 exe 실행 불가 |
| `FastPortServerRIO` | 동일 | Protocols 공유 → 동시 영향 |
| `FastPortClient` | `Tests.pb.h` (FastPortOutboundSession) | 경량 consumer |
| `FastPortTestClient` | `Admin.pb.h`, `Tests.pb.h` (ABCompare, TestSession) | UI 연동 |
| `FastPortBenchmark` | `Benchmark.pb.h` (LatencyBenchmarkRunner) | 성능 측정용 |
| `LibNetworksTests` | transitive (via LibNetworks) | **SC-3 핵심 대상** |
| `LibNetworksRIOTests` | transitive | **SC-3 핵심 대상** |

### 6.3 Verification

- [ ] 8개 consumer 의 링크 산출물에서 `dumpbin /imports <binary> | findstr Protocols` 결과로 `Protocols.dll` import 확인
- [ ] 각 binary 실행 시 `Protocols.dll` 부재 시 Windows loader 오류 재현 (SC-5)
- [ ] 기존 테스트 전체 실행 결과 diff 없음 (SC-4)
- [ ] `Commons.props` 변경이 8 consumer 에 유효하게 적용됐는지 `msbuild /v:d` 로그에서 `/FI` 옵션 존재 확인

---

## 7. Architecture Considerations

> **NOTE**: 본 feature 는 C++ build system refactor 로 템플릿의 웹 프레임워크 섹션 (Next.js/React/bkend.ai 등) 는 적용되지 않음. 대신 C++ build 측면 의사결정만 기록.

### 7.1 Project Level Selection

| Level | Applicable | Selected |
|-------|:---:|:---:|
| Starter | N/A | - |
| Dynamic | N/A | - |
| **Enterprise (C++ system)** | ✅ MSBuild 다중 프로젝트, DI 없음, 네이티브 런타임 | ✅ |

### 7.2 Key Architectural Decisions

| Decision | Options | Selected | Rationale |
|----------|---------|----------|-----------|
| DLL Export 기법 | A) Forced Include `/FI` / B) Per-project Preprocessor / C) Manual header include | **A** | Design §3 확정. 생성 코드 수정 불요, 중앙집중 가능 |
| Export 매크로 중앙화 | 8개 프로젝트 개별 설정 / `Commons.props` 통합 | **`Commons.props` 통합** | R-1 Critical risk 제거. 유지보수 단일 지점 |
| `PROTOCOLS_EXPORTS` 정의 위치 | `Protocols.vcxproj` PreprocessorDefinitions | **동일** | DLL 빌드 측에서만 `dllexport` 경로 활성, consumer 는 자동 `dllimport` |
| CRT | MD/MDd 유지 / MT 전환 | **MD/MDd 유지** | vcpkg triplet 호환, `Commons.props` 이미 강제 |
| Rollback 전략 | (a) 조건부 static fallback / (b) git revert / **(c) 브랜치 격리 + PR 검증** | **(c)** | user 선택. 작업 격리 + 통합 테스트 이후 일괄 머지 |
| Generated code edge case | (a) 수동 patch / (b) LITE_RUNTIME / **(c) `/FI` 우선, 문제 시 판단** | **(c)** | user 선택. 실용적 — 문제 지수 선제 방어 대신 발생 시 대응 |

### 7.3 Build Structure Preview

```
Protocols/
├── ProtocolsAPI.h          ← 신규
├── Protocols.vcxproj       ← 수정 (ConfigurationType=DynamicLibrary)
├── *.proto                 ← 변경 없음
└── *.pb.cc, *.pb.h         ← protoc 재생성 (dllexport_decl 주석 포함)

_Builds/x64/{Debug,Release}/
├── Protocols.dll           ← 신규 출력 (Primary artifact)
├── Protocols.lib           ← 신규 출력 (Import lib)
├── Protocols.pdb
├── LibNetworks.lib         ← 기존 (Protocols.dll 을 transitive 참조)
├── FastPortServer.exe      ← 기존 (Protocols.dll import)
└── ... (나머지 consumer)

Commons.props                ← 수정 (/FI"$(SolutionDir)Protocols\ProtocolsAPI.h" 공통 추가)
```

---

## 8. Branch Strategy

Q1=(c) 선택 반영. 본 전환의 위험 격리를 위해 별도 브랜치 + PR 검증:

1. **브랜치 생성**: `feat/protocols-dll-conversion` from `main`
2. **작업 단위**: M1 → M2 → M3 단일 브랜치 순차 커밋
3. **검증 게이트 (PR 전 필수)**:
   - SC-1 ~ SC-6 전부 green
   - `dumpbin /imports` 로 8개 consumer 에서 `Protocols.dll` import 확인
   - 기존 regression 0
4. **머지 방식**: Squash merge (PDCA 단일 feature 로 이력 정리)
5. **Rollback**: 머지 후 문제 발견 시 `git revert <squash-commit>` — 재빌드만으로 원복

---

## 9. Implementation Plan (Milestones)

Design §5 Session Guide 5 steps + 본 Plan 의 요구사항 반영:

### M1 — Core Conversion (~30 min)

- [ ] `Protocols/ProtocolsAPI.h` 생성 — `PROTOCOLS_EXPORTS` 분기 매크로
- [ ] `Protocols/Protocols.vcxproj` 수정
  - `<ConfigurationType>StaticLibrary</ConfigurationType>` → `DynamicLibrary`
  - `<PreprocessorDefinitions>` 에 `PROTOCOLS_EXPORTS;...` 추가 (Debug/Release 양쪽)
  - `protoc` custom build command 에 `--cpp_out=dllexport_decl=PROTOCOLS_API:$(ProjectDir)` 추가
- [ ] Protocols 단독 빌드 성공 확인 → `Protocols.dll` + `Protocols.lib` 생성

### M2 — Macro Integration (~60 min)

- [ ] `Commons.props` 에 `<AdditionalOptions>/FI"$(SolutionDir)Protocols\ProtocolsAPI.h" %(AdditionalOptions)</AdditionalOptions>` 공통 추가
- [ ] 8개 consumer 순차 빌드:
  1. `LibNetworks` (가장 큰 consumer)
  2. `FastPortServer`, `FastPortServerRIO`
  3. `FastPortClient`, `FastPortTestClient`
  4. `FastPortBenchmark`
  5. `LibNetworksTests`, `LibNetworksRIOTests`
- [ ] 빌드 실패 시 에러 패턴 분류:
  - link 에러 (누락 심볼) → 해당 proto 수동 `__declspec(dllexport)` 주석 보강
  - compile 에러 (매크로 충돌) → `PROTOCOLS_API` 재정의 체크
- [ ] Debug + Release × x64 전체 green

### M3 — Validation (~60 min)

- [ ] `dumpbin /imports _Builds/x64/Debug/LibNetworksTests.dll | findstr Protocols` → `Protocols.dll` import 확인 (8 binary 전부)
- [ ] 개별 test 실행 regression 비교:
  - `vstest.console.exe _Builds/x64/Debug/LibCommonsTests.dll` (37/37 기대)
  - `vstest.console.exe _Builds/x64/Debug/LibNetworksTests.dll` (49/49 기대)
- [ ] **SC-3 통합 실행**: `vstest.console.exe _Builds/x64/Debug/LibNetworksTests.dll _Builds/x64/Debug/LibNetworksRIOTests.dll /Platform:x64` → crash 없이 완료
- [ ] `Protocols.dll` 누락 시나리오 수동 검증 (SC-5)
- [ ] Release 동일 반복
- [ ] 빌드 시간 측정 (baseline 대비 비교, SC-6 optional)

---

## 10. Documentation Updates

- [ ] `docs/BUILD_GUIDE.md` — "배포 시 `Protocols.dll` 동반 배포" 규약 + 빌드 산출물 목록 갱신
- [ ] `docs/PROJECT_STRUCTURE.md` — Protocols 카드 "Static Library" → "Dynamic Library"
- [ ] `docs/ARCHITECTURE_IOCP.md` / `docs/ARCHITECTURE_RIO.md` — 의존성 다이어그램에서 Protocols 를 DLL 노드로 표시 (선택적)
- [ ] `CLAUDE.md` — 새 `.proto` 추가 시 workflow 변경점 (dependent 재빌드 불요, DLL 만 교체) 반영 (선택적)

---

## 11. Next Steps

1. [ ] `docs/02-design/features/protocols-dll-conversion.design.md` 검토 — 본 Plan 기준 5 step → 11 step 으로 세분화 여부 판단 (선택적, 기존 Session Guide 충분)
2. [ ] `feat/protocols-dll-conversion` 브랜치 생성 → `/pdca do protocols-dll-conversion` 으로 M1 착수
3. [ ] PR 머지 후 `/pdca analyze protocols-dll-conversion` 으로 Gap Analysis
4. [ ] 문서 업데이트 + `/pdca report protocols-dll-conversion` 마무리

---

## Version History

| Version | Date | Changes | Author |
|:-------:|------|---------|--------|
| 0.1 | 2026-04-22 | Initial draft — M1/M2/M3 milestones + 3 Success Criteria + 4 Risks | AnYounggun |
| 0.2 | 2026-04-24 | PRD 기반 확장: SC 3→6, Risks 4→6 (severity 추가), Out-of-Scope / Stakeholders / Impact Analysis 섹션 신규, Branch Strategy (Q1=c) + Generated code edge case (Q2=c) 결정 명시, Documentation Updates 목록 추가 | AnYounggun |
