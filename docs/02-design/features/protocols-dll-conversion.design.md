# Design: Protocols DLL Conversion

## Context Anchor
| Dimension | Content |
| :--- | :--- |
| WHY | Prevent Protobuf descriptor registration conflict in single process space |
| WHO | Developers running tests and benchmarks |
| RISK | DLL export symbol missing, binary compatibility (MD vs MT), Runtime DLL missing |
| SUCCESS | `vstest.console` runs all tests from multiple projects in a single run without crash |
| SCOPE | `Protocols` project, all dependent projects (`LibNetworks`, `FastPortServer`, etc.) |

## 1. Overview
This design transforms the `Protocols` project into a Dynamic Link Library (DLL) to ensure that Protobuf descriptors are registered exactly once per process. This solves the fatal crash during test execution when multiple test DLLs (each statically linking `Protocols`) are loaded together.

## 2. Architecture Options

### Option A: Forced Include Header (Recommended)
- **Strategy:** Define `PROTOCOLS_API` macro in a central header and use MSVC's `/FI` (Forced Include File) compiler option.
- **Pros:** 
  - Automatically applies to Protobuf-generated files without modifying them.
  - Minimal manual code changes.
  - Consistent macro definition across all projects.
- **Cons:** 
  - Dependency is hidden in project settings.

### Option B: Project-Level Preprocessor Definitions
- **Strategy:** Define `PROTOCOLS_API=__declspec(dllexport/dllimport)` directly in each project's preprocessor settings.
- **Pros:** 
  - No new header files required.
  - Standard MSVC pattern.
- **Cons:** 
  - Every consumer project must be manually updated with the correct macro.
  - Error-prone if a new project is added.

### Option C: Manual Header Management
- **Strategy:** Create `ProtocolsAPI.h` and manually include it in every source file.
- **Pros:** 
  - Explicit and visible dependencies.
- **Cons:** 
  - High manual effort.
  - **Critical Failure:** Cannot easily include the header in Protobuf-generated `.pb.h` files without modifying the generation script or using forced includes anyway.

## 3. Comparison & Trade-offs

| Criterion | Option A (Forced) | Option B (Macro) | Option C (Manual) |
| :--- | :--- | :--- | :--- |
| **Effort** | Low | Medium | High |
| **Maintainability** | High | Medium | Low |
| **Reliability** | High | High | Low |
| **Complexity** | Low | Low | Medium |

**Recommendation:** **Option A** is the most robust and low-maintenance approach for projects with generated code.

## 4. Implementation Details

### 4.1. ProtocolsAPI.h
```cpp
#pragma once

#ifdef PROTOCOLS_EXPORTS
    #define PROTOCOLS_API __declspec(dllexport)
#else
    #define PROTOCOLS_API __declspec(dllimport)
#endif
```

### 4.2. Protocols.vcxproj Changes
- Change `<ConfigurationType>` to `DynamicLibrary`.
- Add `PROTOCOLS_EXPORTS` to `PreprocessorDefinitions`.
- Update `protoc` command:
  `--cpp_out=dllexport_decl=PROTOCOLS_API:$(ProjectDir)`
- Add `/FI"ProtocolsAPI.h"` to `AdditionalOptions`.

### 4.3. Dependent Projects Changes
- Add `/FI"ProtocolsAPI.h"` to `AdditionalOptions`.
- Ensure `Protocols` is in the include path (already in `Commons.props`).

## 5. Session Guide (Implementation Steps)
1. Create `Protocols/ProtocolsAPI.h`.
2. Modify `Protocols/Protocols.vcxproj`:
   - Set DLL type.
   - Update `protoc` command.
   - Add `PROTOCOLS_EXPORTS`.
   - Add Forced Include.
3. Update `Commons.props` (optional) or individual projects to include `ProtocolsAPI.h`.
4. Build and verify `Protocols.dll` and `Protocols.lib`.
5. Rebuild dependent projects.
