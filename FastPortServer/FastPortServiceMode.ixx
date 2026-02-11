module;

#include <windows.h>
#include <stdint.h>
export module fastport_service_mode;

import std; 
import commons.service_mode;
import networks.core.socket; 
import networks.core.io_socket_acceptor;
import networks.services.rio_service;
import networks.core.rio_buffer_manager;

export class FastPortServiceMode : public LibCommons::ServiceMode
{
public:
    FastPortServiceMode() : ServiceMode(true, true, false) {}

    // 명령줄 인수를 파싱하여 서비스 모드(IOCP/RIO) 설정
    bool ParseArgs(int argc, const char* argv[]);

protected:
    // 서비스 시작 시 호출되어 선택된 네트워크 모드로 서버 구동
    void OnStarted() override;

    // 서비스 중지 시 호출되어 RIO 서비스 중단
    void OnStopped() override;

    // 서비스 종료 시 호출되어 리소스를 정리하고 소켓 닫기
    void OnShutdown() override;

    // 서비스 이름 반환
    std::wstring GetServiceName() const override { return L"FastPortServer"; }

    // 서비스 표시 이름 반환
    std::wstring GetDisplayName() override { return L"FastPortServer Service"; }

    // 서비스 시작 유형 반환
    const DWORD GetStartType() const override { return SERVICE_DEMAND_START; }

private:
    // IOCP 모드로 서버 시작
    void StartIocpMode();

    // RIO 모드로 서버 시작
    void StartRioMode();

private:
    // 서버 리스닝 포트
    const unsigned short C_LISTEN_PORT = 6628;
    // RIO 수신 버퍼 크기 (16KB)
    const uint32_t C_RIO_RECV_BUFFER_SIZE = 16 * 1024;
    // RIO 송신 버퍼 크기 (16KB)
    const uint32_t C_RIO_SEND_BUFFER_SIZE = 16 * 1024;

    // 네트워크 모드 (기본값 IOCP)
    LibNetworks::Core::Socket::ENetworkMode m_NetworkMode = LibNetworks::Core::Socket::ENetworkMode::IOCP;

    // 리스닝 소켓
    LibNetworks::Core::Socket m_ListenSocket{};

    // IOCP 소켓 수락 처리기
    std::shared_ptr<LibNetworks::Core::IOSocketAcceptor> m_Acceptor{};

    // RIO 서비스 관리자
    std::shared_ptr<LibNetworks::Services::RIOService> m_RioService{};

    // RIO 버퍼 관리자
    std::shared_ptr<LibNetworks::Core::RioBufferManager> m_RioBufferManager{};
};