module;

#include <Windows.h>
#include <spdlog/spdlog.h>
module networks.services.io_service;

import commons.logger;
import commons.rwlock; 
import networks.core.io_consumer;

namespace LibNetworks::Services
{


IOService::IOService()
{

}

IOService::~IOService()
{
    if (INVALID_HANDLE_VALUE != m_hICOP)
    {
        ::CloseHandle(m_hICOP);
        m_hICOP = INVALID_HANDLE_VALUE;
    }
}

bool IOService::Start(unsigned int numThreads)
{
    auto& logger = LibCommons::Logger::GetInstance();

    if (!CreateCompletionPort())
    {
        logger.LogError("IOService", "Start, CreateCompletionPort failed.");
        return false;
    }

    m_bTerminated.store(false, std::memory_order_acquire);

    auto fDoWorker = [this, &logger]()
        {
            while (true)
            {
                DWORD bytesTransferred = 0;
                ULONG_PTR completionId = 0;
                OVERLAPPED* pOverlapped = nullptr;

                BOOL bResult = ::GetQueuedCompletionStatus(m_hICOP, &bytesTransferred, &completionId, &pOverlapped, INFINITE);
                if (!bResult)
                {
                    DWORD dwError = ::GetLastError();
                    if (ERROR_ABANDONED_WAIT_0 == dwError || ERROR_CONNECTION_ABORTED == dwError)
                    {
                        // IOCC 정상종료
                        logger.LogError("IOService", "Worker thread, IOCP handle closed or operation aborted. Error : {}", dwError);

                        break;
                    }

                    if (ERROR_NETNAME_DELETED == dwError || ERROR_CONNECTION_ABORTED == dwError)
                    {
                        logger.LogError("IOService", "Worker thread, Connection closed. Error: {}", dwError);

                        auto pConsumer = reinterpret_cast<Core::IIOConsumer*>(completionId);
                        if (pConsumer)
                        {
                            pConsumer->OnIOCompleted(false, bytesTransferred, pOverlapped);
                        }

                        continue;
                    }

                    logger.LogError("IOService", "Worker thread, GetQueuedCompletionStatus failed. Error: {}", dwError);
                }

                if (C_THREAD_SHUTDOWN_COMPLETION_KEY == completionId)
                {
                    logger.LogInfo("IOService", "Worker thread, Shutdown signal received. Exiting thread.");
                    break;
                }

                auto pConsumer = reinterpret_cast<Core::IIOConsumer*>(completionId);
                if (pConsumer)
                {
                    pConsumer->OnIOCompleted(bResult == TRUE, bytesTransferred, pOverlapped);
                }
            }
        };

    for (size_t i = 0; i < numThreads; i++)
    {
        m_Workers.emplace_back(fDoWorker);
    }

    return false;
}

void IOService::Stop()
{
    for (size_t i = 0; i < m_Workers.size(); ++i)
    {
        Post(C_THREAD_SHUTDOWN_COMPLETION_KEY);
    }

    for (auto& thread : m_Workers)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    m_Workers.clear();

    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_bTerminated.store(true, std::memory_order_release);
    }

    m_cv.notify_all();
}

void IOService::Wait()
{
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        m_cv.wait(lock, [this]()
            {
                return m_bTerminated.load(std::memory_order_acquire);
            }
        );
    }

    LibCommons::Logger::GetInstance().LogInfo("IOService", "Service wait completed.");
}

bool IOService::Associate(SOCKET& rfSocket, ULONG_PTR completionId) const
{
    auto& logger = LibCommons::Logger::GetInstance();

    HANDLE hResult = ::CreateIoCompletionPort(reinterpret_cast<HANDLE>(rfSocket), m_hICOP, completionId, 0);
    if (hResult == nullptr)
    {
        logger.LogError("IOService", "Associate failed. Error: {}", ::GetLastError());

        return false;
    }

    if (hResult != m_hICOP)
    {
        logger.LogError("IOService", "Associate returned unexpected handle");
        return false;
    }

    logger.LogInfo("IOService", "Associate, Socket associated successfully. CompletionId : {}", completionId);

    return true;
}

bool IOService::Post(ULONG_PTR uCompletionKey, DWORD bytes, OVERLAPPED* ov) const
{
    if (!::PostQueuedCompletionStatus(m_hICOP, bytes, uCompletionKey, ov))
    {
        LibCommons::Logger::GetInstance().LogError("IOService", "Post failed. Error: {}", ::GetLastError());
        return false;
    }
    return true;
}

bool IOService::CreateCompletionPort()
{
    m_hICOP = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (m_hICOP == nullptr)
    {
        LibCommons::Logger::GetInstance().LogError("IOService", "CreateCompletionPort, CreateIoCompletionPort failed. Last Error : {}", ::GetLastError());

        return false;
    }

    return true;

}



} // namespace LibNetworks::Services