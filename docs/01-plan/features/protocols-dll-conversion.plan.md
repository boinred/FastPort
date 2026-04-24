# Plan: Protocols DLL Conversion

## 1. Executive Summary
Protobuf descriptor conflict ("File already exists in database") occurs when multiple static libraries containing the same proto descriptors are loaded into a single process (e.g., `vstest.console.exe`). This plan converts the `Protocols` project from a Static Library to a Dynamic Library (DLL) to ensure that descriptors are registered only once in the process space.

### Value Delivered
| Problem | Solution | Function UX Effect | Core Value |
| :--- | :--- | :--- | :--- |
| Test runner crash due to double registration of proto descriptors | Convert `Protocols` to DLL | Stable test environment, single instance of descriptors | Reliability, Build Efficiency |

## Context Anchor
| Dimension | Content |
| :--- | :--- |
| WHY | Prevent Protobuf descriptor registration conflict in single process space |
| WHO | Developers running tests and benchmarks |
| RISK | DLL export symbol missing, binary compatibility (MD vs MT), Runtime DLL missing |
| SUCCESS | `vstest.console` runs all tests from multiple projects in a single run without crash |
| SCOPE | `Protocols` project, all dependent projects (`LibNetworks`, `FastPortServer`, etc.) |

## 2. Requirements
- Convert `Protocols` project to Dynamic Library (DLL).
- Correctly export all generated Protobuf classes and functions.
- Ensure all dependent projects link to the import library (.lib).
- Maintain existing build pipeline (automatic proto compilation).

## 3. Risks & Constraints
- **Symbol Export:** Protobuf generated code needs `dllexport_decl` for DLL export.
- **Runtime Dependency:** DLL must be in the search path (already handled by unified `OutDir`).
- **ABI Compatibility:** All projects must use the same CRT (currently `MD/MDd` is standard for vcpkg).
- **Maintenance:** Adding new proto files requires manual registration in the project if not using globbing (current project uses explicit list).

## 4. Proposed Strategy
1. **Define Export Macro:** Create `ProtocolsAPI.h` and define `PROTOCOLS_API` for dllexport/dllimport.
2. **Update Protocols Project:** Change `ConfigurationType` to `DynamicLibrary`.
3. **Update Build Event:** Modify `protoc` command to include `--cpp_out=dllexport_decl=PROTOCOLS_API:...`.
4. **Apply Forced Include:** Use `/FI "ProtocolsAPI.h"` in `Protocols` and dependent projects to ensure the macro is defined before any `.pb.h` is included.
5. **Verify Dependencies:** Ensure all dependent projects correctly reference the DLL via `ProjectReference`.

## 5. Success Criteria
- [ ] `Protocols.dll` and `Protocols.lib` are generated.
- [ ] Dependent projects build successfully.
- [ ] `vstest.console.exe` can run `LibNetworksTests.dll` and other test DLLs in a single process without crashing.

## 6. Implementation Plan (Milestones)
- **M1: Core Conversion** - Modify `Protocols.vcxproj` and `protoc` command.
- **M2: Macro Integration** - Add `ProtocolsAPI.h` and configure forced includes.
- **M3: Validation** - Run multi-DLL test suite.
