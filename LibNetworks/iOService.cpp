module; 

#include <Windows.h>

module networks.services.io_service;

import commons.logger;
import commons.rwlock; 

namespace LibNetworks::Services
{
IOService::IOService()
{

}

bool IOService::Start(unsigned int numThreads)
{
    return false;
}

void IOService::Stop()
{
}

bool IOService::Associate(SOCKET& rfSocket, ULONG_PTR ulCompletionKey)
{
    return false;
}

bool IOService::Post(ULONG_PTR uCompletionKey, DWORD bytes, OVERLAPPED* ov)
{
    return false;
}

bool IOService::CreateCompletionPort()
{
	m_hICOP = ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);

	if (m_hICOP == nullptr)
	{
		LibCommons::RWLock lock;
		lock.ReadLock(); 
	
		//LibCommons::Logger::GetInstance().LogError("IOService", "CreateCompletionPort, CreateIoCompletionPort failed. Last Error : {}", ::GetLastError());

		//LibCommons::Logger::GetInstance().LogInfo("FastPortClient", "Test logger.");

		return false;
	}

	return true;
    
}


} // namespace LibNetworks::Services