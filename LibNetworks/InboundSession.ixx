module;

#include <memory>
export module networks.sessions.inbound_session;

import networks.sessions.io_session;
import networks.core.socket; 

namespace LibNetworks::Sessions
{
// [Inbound] 리스너가 Accept하여 생성된 세션(남이 나한테 옴)
// 예: OnConnected()에서 클라이언트 환영 메시지 전송 등
export class InboundSession : public IOSession
{
public:
    InboundSession() = delete;
    InboundSession(const InboundSession&) = delete;
    InboundSession& operator=(const InboundSession&) = delete;

    virtual ~InboundSession() = default;

    explicit InboundSession(const std::shared_ptr<Core::Socket>& pSocket);

    void OnAccepted() override;

    void OnDisconnected() override;

};

} // namespace LibNetworks::Sessions