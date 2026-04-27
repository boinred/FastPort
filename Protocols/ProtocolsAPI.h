// Design Ref: §4.1 ProtocolsAPI.h — Protocols DLL export macro (Option A Forced Include)
// Plan SC: SC-1 ~ SC-5 — DLL export / import 분기 진입점
//
// 본 헤더는 MSVC `/FI"ProtocolsAPI.h"` Forced Include 로 Protocols 프로젝트 및 모든
// consumer 프로젝트에 주입된다 (Commons.props 중앙집중). Protobuf 생성 코드가
// `--cpp_out=dllexport_decl=PROTOCOLS_API:` 옵션으로 주석 삽입된 상태에서 본 매크로를
// 만나 dllexport (DLL 빌드 측) 또는 dllimport (consumer 측) 로 확장된다.
//
// `PROTOCOLS_EXPORTS` 는 Protocols.vcxproj 의 PreprocessorDefinitions 에서만 정의되며,
// consumer 프로젝트에는 정의되지 않아 자동으로 dllimport 경로로 분기한다.

#pragma once

#ifdef PROTOCOLS_EXPORTS
    #define PROTOCOLS_API __declspec(dllexport)
#else
    #define PROTOCOLS_API __declspec(dllimport)
#endif
