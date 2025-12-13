module;
#include <windows.h>
export module networks.services.io_consumer;

namespace LibNetworks::Services
{

export class IIOConsumer
{
public:

	virtual void OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped) = 0;

	virtual ULONG_PTR GetCompletionId() const
	{
		return reinterpret_cast<ULONG_PTR>(this);
	}
};

} // export namespace LibNetworks::Services