module;

#include <WinSock2.h>
#include <MSWSock.h>
#include <vector>
#include <thread>
#include <atomic>

export module networks.services.rio_service;

import networks.core.rio_extension;
import networks.services.inetwork_service;

namespace LibNetworks::Services
{

/**
 * RIO 완료 통지를 처리하는 서비스 클래스
 */
export class RIOService : public INetworkService
{
public:
    RIOService() = default;
    virtual ~RIOService() override;

    // RIO Completion Queue를 생성하고 서비스를 초기화합니다.
    bool Initialize(uint32_t maxCompletionResults);

    // 서비스를 시작합니다 (워커 스레드 생성).
    virtual bool Start(uint32_t threadCount) override;

    // 서비스를 중지합니다.
    virtual void Stop() override;

    RIO_CQ GetCompletionQueue() const { return m_CQ; }

private:
    // 워커 스레드 함수
    void WorkerLoop();

    // 개별 작업 결과 처리
    static void ProcessResult(const RIORESULT& result);

private:
    RIO_CQ m_CQ = RIO_INVALID_CQ;
    std::vector<std::thread> m_WorkerThreads;
    std::atomic<bool> m_bIsRunning = false;
};

} // namespace LibNetworks::Core