module;

#include <cstdint>

export module networks.services.inetwork_service;

namespace LibNetworks::Services
{

/**
 * 네트워크 서비스 인터페이스 (IOCP, RIO 공통)
 */
export class INetworkService
{
public:
    virtual ~INetworkService() = default;

    // 서비스를 시작합니다.
    virtual bool Start(uint32_t threadCount) = 0;

    // 서비스를 중지합니다.
    virtual void Stop() = 0;
};

} // namespace LibNetworks::Services
