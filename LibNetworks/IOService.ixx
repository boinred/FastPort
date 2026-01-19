module;
#include <windows.h>
#include <vector>
#include <thread>
#include <mutex>

export module networks.services.io_service;

namespace LibNetworks::Services
{
export class IOService
{
public:
    IOService();
    ~IOService();

    IOService(const IOService&) = delete;

    bool Start(unsigned int numThreads);

    void Stop();

    void Wait();

    bool Associate(SOCKET& rfSocket, ULONG_PTR completionId) const;

    bool Post(ULONG_PTR uCompletionKey, DWORD bytes = 0, OVERLAPPED* ov = nullptr) const;

private:
    bool CreateCompletionPort();
private:

    std::vector<std::thread> m_Workers;

    const ULONG_PTR C_THREAD_SHUTDOWN_COMPLETION_KEY = 0xFFFFFFFFFFFFFFFF;

    HANDLE m_hICOP = nullptr;

    std::mutex m_Mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_bTerminated = false;
};


} // namespace LibNetworks::Services