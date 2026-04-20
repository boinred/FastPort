// RIOServiceMode.ixx
// -----------------------------------------------------------------------------
// Windows Service 인터페이스와 RIO(Registered I/O) 네트워크 스택을 연결하는 서비스 모드.
//
// 역할:
//   - LibCommons::ServiceMode 를 상속하여 Windows Service 생명주기(OnStarted / OnStopped /
//     OnShutdown) 에 RIO 인프라 구성/정리를 구현.
//   - RIO 확장(RioExtension), RIO 서비스 루프(RIOService), RIO 고정 버퍼 풀
//     (RioBufferManager), 수신 Acceptor(IOSocketAcceptor) 를 일괄 소유.
//   - 새 연결이 들어오면 RioBufferSlice 2개(수신/송신)를 할당해 RIOInboundSession 생성.
//
// Listen 포트:
//   - 6628 (IOCP 와 다른 포트 사용으로 동시 실행 가능)
//
// 관련 파일:
//   - RIOServiceMode.cpp       : OnStarted/OnStopped/OnShutdown 구현
//   - RIOInboundSession.{ixx,cpp} : 개별 연결 세션
//   - LibCommons::ServiceMode  : Windows Service 프레임워크
//   - LibNetworks::Services::RIOService : RIO CompletionQueue 루프
// -----------------------------------------------------------------------------
module;

#include <windows.h>
#include <winnt.h>
#include <stdint.h>
export module rio_service_mode;

import std;
import commons.service_mode;
import networks.core.socket;
import networks.core.io_socket_acceptor;
import networks.services.rio_service;
import networks.core.rio_buffer_manager;
import rio_inbound_session;


// RIO 모드 서비스 엔트리. IOCPServiceMode 와 동일 인터페이스를 제공하지만 내부 구성은 완전히 다름.
// - ctor 플래그: canStop=true, canShutdown=true, canPauseContinue=false.
export class RIOServiceMode : public LibCommons::ServiceMode
{
public:
    RIOServiceMode() : ServiceMode(true, true, false) {}

protected:
    // 서비스 시작 시: RIO 확장 로드 → RIOService 초기화 → BufferManager 초기화 → Acceptor 생성.
    void OnStarted() override;

    // 서비스 정지 요청: RIOService 루프를 먼저 내려 완료 이벤트가 더 이상 들어오지 않게 함.
    void OnStopped() override;

    // 시스템 종료: Acceptor 를 닫아 신규 연결을 차단.
    void OnShutdown() override;

    // Windows Service 등록 시 사용되는 이름. FastPortServer(IOCP) 와 구분되도록 별도.
    std::wstring GetServiceName() const override { return L"FastPortServerRIO"; }
    std::wstring GetDisplayName() override { return L"FastPortServer RIO Service"; }
    const DWORD GetStartType() const override { return SERVICE_DEMAND_START; }

private:
    // 수신 대기 포트. IOCP(6627 등)와 겹치지 않게 별도 할당.
    const unsigned short C_LISTEN_PORT = 6628;

    // 세션당 할당되는 RIO 고정 버퍼 크기 (수신/송신 각각).
    // RIO 는 커널에 등록된 버퍼만 사용 가능하므로 RioBufferManager 에서 pre-allocate 풀로 관리.
    const uint32_t C_RIO_RECV_BUFFER_SIZE = 16 * 1024;
    const uint32_t C_RIO_SEND_BUFFER_SIZE = 16 * 1024;

    // 리스닝용 서버 소켓.
    LibNetworks::Core::Socket m_ListenSocket{};

    // 새 연결을 AcceptEx 로 받고 OnFuncCreateSession 콜백으로 세션 생성.
    std::shared_ptr<LibNetworks::Core::IOSocketAcceptor> m_Acceptor{};

    // RIO CompletionQueue + 워커 스레드(들)를 구동하는 서비스 루프.
    std::shared_ptr<LibNetworks::Services::RIOService> m_RioService{};

    // 프로세스 전역 RIO 버퍼 풀. 새 세션에게 slice 를 할당해 준다.
    std::shared_ptr<LibNetworks::Core::RioBufferManager> m_RioBufferManager{};
};
