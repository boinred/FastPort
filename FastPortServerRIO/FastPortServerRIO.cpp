// FastPortServerRIO.cpp
// -----------------------------------------------------------------------------
// FastPort RIO(Registered I/O) 전용 서버의 엔트리 포인트.
//
// FastPortServer 프로젝트와 분리된 이유:
//   - IOCP 와 RIO 는 서로 다른 I/O 모델(완료 포트 vs 등록된 I/O)을 사용하며,
//     내부 구현(세션·서비스)이 완전히 다름.
//   - 하나의 실행 파일에 `--rio` 플래그로 분기하던 이전 구조는 유지보수/테스트 분리가
//     어려워 독립 프로젝트로 분리.
//
// 배포 산출물:
//   - FastPortServer.exe    (IOCP)
//   - FastPortServerRIO.exe (이 프로젝트)
//
// 동일 솔루션의 LibCommons / LibNetworks / Protocols 에 의존하며, 로깅/이벤트
// 리스너/소켓 초기화 순서는 IOCP 서버와 동일.
// -----------------------------------------------------------------------------

#include <iostream>
#include <filesystem>
#include <spdlog/spdlog.h>
#include <thread>
#include <chrono>

import std;
import commons.logger;
import commons.event_listener;
import commons.service_mode;
import networks.core.socket;

// RIO 전용 서비스 모드. OnStarted/OnStopped/OnShutdown 생명주기로 RIO 초기화/정리 수행.
import rio_service_mode;


int main(int argc, const char* argv[])
{
    // 현재 작업 디렉터리(보통 빌드 출력 폴더)를 기준으로 로그 경로 구성.
    std::string location = std::filesystem::current_path().string();

    // 1) 로거 초기화
    //    Release 빌드에서는 Windows Service 모드로, Debug 빌드에서는 콘솔 포함 모드로 실행.
    bool bServiceMode = true;
#if _DEBUG
    bServiceMode = false;
#endif

    // 파일명 형식: log_YYYY_MM_DD_HH_MM.txt — 분 단위 회전으로 실행 세션 구분 용이.
    auto now = std::chrono::floor<std::chrono::minutes>(std::chrono::current_zone()->to_local(std::chrono::system_clock::now()));
    std::string fileName = std::format("log_{:%Y_%m_%d_%H_%M}.txt", now);

    auto& logger = LibCommons::Logger::GetInstance();
    logger.Create(location + "/" + "loggers", fileName, 1024 * 1024 * 10, 3, bServiceMode);

    // 2) 네트워크 전역 초기화 (WSAStartup 등) + 이벤트 리스너 스레드 풀 준비.
    LibNetworks::Core::Socket::Initialize();
    LibCommons::EventListener::GetInstance().Init(std::thread::hardware_concurrency());

    // 3) RIO 서비스 실행.
    //    RIOServiceMode::OnStarted 가 RIO 확장 로드 + RIOService + RioBufferManager + Acceptor 구성.
    auto pService = std::make_shared<RIOServiceMode>();

    logger.LogInfo("Main", "FastPortServerRIO Starting...");

    // ServiceMode::Execute 는 Windows Service / 콘솔 두 모드 모두 처리.
    pService->Execute(static_cast<DWORD>(argc), argv);

#if _DEBUG
    // Debug 빌드에선 서비스 제어 매니저가 없으므로 수동으로 종료 대기.
    pService->Wait();
#endif

    logger.LogInfo("Main", "FastPortServerRIO Closed.");
    return 0;
}
