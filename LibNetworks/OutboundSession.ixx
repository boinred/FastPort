module;
#include <memory>
export module networks.sessions.outbound_session;

import networks.sessions.io_session;
import networks.core.socket;

namespace LibNetworks::Sessions
{
// [Outbound] 커넥터가 Connect하여 생성된 세션 (내가 남한테 감)
// 예: OnConnected()에서 로그인 패킷 전송 등
export class OutboundSession : public IOSession
{
public:
    OutboundSession() = delete;
    OutboundSession(const OutboundSession&) = delete;
    OutboundSession& operator=(const OutboundSession&) = delete;

    virtual ~OutboundSession() = default;

    explicit OutboundSession(const std::shared_ptr<Core::Socket>& pSocket);

private:
    void OnConnected() override;

    void OnDisconnected() override;

};

} // namespace LibNetworks::Sessions