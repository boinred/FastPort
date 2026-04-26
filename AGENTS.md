# FastPort

## 프로젝트 개발 지침

### 응답 언어

- 사용자가 별도로 요청하지 않으면 모든 설명, 진행 상황, 최종 답변은 한글로 작성한다.
- 코드, 명령어, 파일 경로, API 이름, 로그 원문은 원래 표기를 유지한다.

### 로깅 — LibCommons::Logger 사용

프로젝트 표준 로거는 `LibCommons::Logger` (spdlog 래퍼, category 자동 등록). 모든 로그는 이걸
사용. spdlog 직접 호출(`spdlog::info` 등) 금지.

1. **초기화 필수**: 엔트리 포인트(main, WinMain, 서비스 시작, 테스트 모듈 초기화)에서 반드시 1회
   `Logger::GetInstance().Create(directoryName, fileName, maxFileSize, maxFileCount, bServiceMode)`
   호출. 미초기화 시 spdlog async thread pool 이 없어 `"async log: thread pool doesn't exist anymore"`
   오류 + 로그 누락/크래시 가능.

2. **종료 순서 주의**: `Logger::Shutdown()` 은 spdlog 전체를 종료하므로, 여전히 로그를 쏘는
   객체(예: `TimerQueue` 싱글톤) 들을 **먼저** 정리한 후 호출. 반대 순서는 segfault 유발.

3. **모듈 구현 단위(`.cpp` with `module xxx;`) 에서 Logger 사용 — GMF 에 spdlog 헤더 반드시 포함**:

   `LibCommons::Logger::LogXxx` 는 가변 템플릿이며 spdlog 타입(`spdlog::string_view_t`,
   `spdlog::fmt_lib::runtime` 등)을 직접 참조한다. `import commons.logger;` 만 하면 모듈 경계에서
   spdlog 심볼이 완전히 노출되지 않아 템플릿 인스턴스화 시 **MSVC C1001 ICE** 발생
   (VC Tools `14.50.35717` 재현 확인, `msc1.cpp line 1635`, IFC 가져오기 컨텍스트).

   **해결책** (적용 사례: `LibCommons/TimerQueue.cpp`):
   ```cpp
   module;
   #include <Windows.h>
   #include <spdlog/spdlog.h>   // ← 반드시 GMF 에 포함. 이것만으로 ICE 해결.
   module commons.timer_queue;
   import std;
   import commons.logger;       // Logger 자체는 module 로 import OK.
   // ... 이후 Logger::GetInstance().LogInfo/Warning/Error/Debug(...) 자유롭게 사용 가능.
   ```

   **호출 컨벤션** (템플릿 인스턴스화 최소화):
   - 호출부에서 `std::format` 으로 메시지를 구성한 뒤 Logger 에 `(category, std::string)` 형태로 전달
   - 카테고리는 파일/모듈별로 `constexpr const char* kLogCategory = "XXX";` 정의해 사용
   - 헬퍼 래퍼 사용 (LogXxxInfo/Warning/Error/Debug) 로 호출부 간결화 권장

   예시:
   ```cpp
   // TimerQueue.cpp anonymous namespace
   constexpr const char* kLogCategory = "TimerQueue";
   inline void LogTQInfo(const std::string& msg)    { Logger::GetInstance().LogInfo(kLogCategory, msg); }
   inline void LogTQWarning(const std::string& msg) { Logger::GetInstance().LogWarning(kLogCategory, msg); }
   inline void LogTQError(const std::string& msg)   { Logger::GetInstance().LogError(kLogCategory, msg); }
   inline void LogTQDebug(const std::string& msg)   { Logger::GetInstance().LogDebug(kLogCategory, msg); }
   // 호출부:
   LogTQError(std::format("CreateThreadpoolTimer failed. GLE : {}", ::GetLastError()));
   ```

### 스레드풀 타이머 콜백 수명 — Critical

`LibCommons::TimerQueue` (및 일반 비동기 콜백)를 사용할 때:

1. **지역 변수 ref 캡처 + periodic 예약 금지**: 함수가 반환하면 캡처된 지역 변수는 해제됨.
   콜백이 계속 발사되면 dangling ref → 스택 오염(다른 변수 영역 변조) 유발. 디버그 빌드에서
   `Run-Time Check Failure #2 - Stack around the variable 'X' was corrupted` 로 드러남.

2. **필수 원칙**:
   - periodic 타이머는 반드시 `Cancel(id)` 또는 `ScopedTimer` RAII 로 정리 후 함수 종료
   - 장기 수명이 필요하면 캡처 대상을 `shared_ptr` / static / heap 으로 배치
   - 싱글톤 `TimerQueue` 사용 테스트는 각 테스트 끝에 예약한 타이머 모두 Cancel

3. **테스트 모듈 cleanup**: `TEST_MODULE_CLEANUP` 에서 `TimerQueue::GetInstance().Shutdown(true)`
   를 Logger Shutdown **전에** 호출. 역순이면 잔여 콜백이 닫힌 spdlog 에 접근해 segfault.

### C++ 표준 / 모듈 / 툴체인

- **언어 표준**: `/std:c++20` (`Commons.props:14` — `stdcpp20`). `std::move_only_function` 등
  C++23 기능이 필요해서 `stdcpplatest` 로 올렸더니 **FastPortServer 의 표준 라이브러리 내부**에서
  무관한 C1001 ICE 발생. 현재는 롤백 상태. C++23 기능은 해당 모듈이 독립적으로 상향 가능할 때까지 보류.
- **플랫폼**: x64 전용. Win32 빌드는 PCH(`pch.h`) 사용하지만 x64 는 비활성.
- **모듈**: `commons.snake_case` 네이밍. GMF 에서 Windows/서드파티 헤더 include → `export module ...`
  → `import std; import commons.xxx;` 순.

### 네이밍 컨벤션

- Class/Struct/Method: `PascalCase`
- Member: `m_` + `PascalCase` (bool은 `m_b`, 포인터는 `m_p`)
- Static/const: `k` + `PascalCase`
- Win32 API: `::` 전역 접두 (`::CreateThreadpoolTimer`, `::GetLastError`)
- Mutex lock: `std::lock_guard<std::mutex>` / `std::unique_lock<std::mutex>` 또는 프로젝트
  유틸 `LibCommons::RWLock` + `ReadLockBlock/WriteLockBlock`

## Skill routing

When the user's request matches an available skill, ALWAYS invoke it using the Skill
tool as your FIRST action. Do NOT answer directly, do NOT use other tools first.
The skill has specialized workflows that produce better results than ad-hoc answers.

Key routing rules:
- Product ideas, "is this worth building", brainstorming → invoke office-hours
- Bugs, errors, "why is this broken", 500 errors → invoke investigate
- Ship, deploy, push, create PR → invoke ship
- QA, test the site, find bugs → invoke qa
- Code review, check my diff → invoke review
- Update docs after shipping → invoke document-release
- Weekly retro → invoke retro
- Design system, brand → invoke design-consultation
- Visual audit, design polish → invoke design-review
- Architecture review → invoke plan-eng-review
- Save progress, checkpoint, resume → invoke checkpoint
- Code quality, health check → invoke health
