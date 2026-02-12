module;

#include <WinSock2.h>
#include <MSWSock.h>
#include <thread>
#include <spdlog/spdlog.h>

module networks.services.rio_service;

import networks.core.rio_extension;
import networks.core.rio_context;
import networks.sessions.rio_session;
import commons.logger;

namespace LibNetworks::Services
{

RIOService::~RIOService()
{
    Stop();
}

bool RIOService::Initialize(uint32_t maxCompletionResults)
{
    if (m_CQ != RIO_INVALID_CQ)
    {
        LibCommons::Logger::GetInstance().LogWarning("RIOService", "Initialize called but Completion Queue already exists. Ignoring.");
        return true;
    }

    m_CQ = Core::RioExtension::GetTable().RIOCreateCompletionQueue(maxCompletionResults, nullptr);
    if (m_CQ == RIO_INVALID_CQ)
    {
        LibCommons::Logger::GetInstance().LogError("RIOService", "Failed to create RIO Completion Queue.");

        return false;
    }

    return true;
}

bool RIOService::Start(uint32_t threadCount)
{
    if (m_bIsRunning.exchange(true))
    {
        return true;
    }

    for (uint32_t i = 0; i < threadCount; ++i)
    {
        m_WorkerThreads.emplace_back(&RIOService::WorkerLoop, this);
    }

    LibCommons::Logger::GetInstance().LogInfo("RIOService", "Started with {} worker threads.", threadCount);

    return true;
}

void RIOService::Stop()
{
    if (!m_bIsRunning.exchange(false))
    {
        return;
    }

    for (auto& thread : m_WorkerThreads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    m_WorkerThreads.clear();

    if (m_CQ != RIO_INVALID_CQ)
    {
        Core::RioExtension::GetTable().RIOCloseCompletionQueue(m_CQ);
        m_CQ = RIO_INVALID_CQ;
    }
}

void RIOService::WorkerLoop()
{
    constexpr uint32_t MAX_RESULTS = 128;
    RIORESULT results[MAX_RESULTS] = {};

    while (m_bIsRunning)
    {
        ULONG count = Core::RioExtension::GetTable().RIODequeueCompletion(m_CQ, results, MAX_RESULTS);

        if (count == 0)
        {
            std::this_thread::yield();
            continue;
        }

        if (count == RIO_CORRUPT_CQ)
        {
            break;
        }

        for (ULONG i = 0; i < count; ++i)
        {
            RIOService::ProcessResult(results[i]);
        }
    }
}

void RIOService::ProcessResult(const RIORESULT& result)
{
    Core::RioContext* pContext = reinterpret_cast<Core::RioContext*>(result.RequestContext);
    if (pContext == nullptr || pContext->pSession == nullptr)
    {
        LibCommons::Logger::GetInstance().LogError("RIOService", "Request Context is not valid.");
        return;
    }

    LibCommons::Logger::GetInstance().LogInfo("RIOService", "Processing RIO result: Status : {}, BytesTransferred : {}, Operation Type : {}", 
        result.Status, result.BytesTransferred, (pContext->OpType == Core::RioOperationType::Receive ? "Receive" : "Send"));

    LibNetworks::Sessions::RIOSession* pSession = reinterpret_cast<LibNetworks::Sessions::RIOSession*>(pContext->pSession);

    bool bSuccess = (result.Status >= 0);
    pSession->OnRioIOCompleted(bSuccess, result.BytesTransferred, pContext->OpType);
}

} // namespace LibNetworks::Services
