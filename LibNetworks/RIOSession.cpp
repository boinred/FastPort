module;

#include <WinSock2.h>
#include <MSWSock.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include <span>

module networks.sessions.rio_session;

import commons.logger;

namespace LibNetworks::Sessions
{

RIOSession::RIOSession(const std::shared_ptr<Core::Socket>& pSocket, const Core::RioBufferSlice& recvSlice, const Core::RioBufferSlice& sendSlice, RIO_CQ completionQueue)
    : m_pSocket(pSocket), m_RecvSlice(recvSlice), m_SendSlice(sendSlice)
{
    m_RQ = Core::RioExtension::GetTable().RIOCreateRequestQueue(
        pSocket->GetSocket(),
        1, 1, 1, 1,
        completionQueue,
        completionQueue,
        this
    );

    m_RecvContext.OpType = Core::RioOperationType::Receive;
    m_RecvContext.pSession = this;

    m_SendContext.OpType = Core::RioOperationType::Send;
    m_SendContext.pSession = this;

    m_pReceiveBuffer = std::make_unique<LibCommons::Buffers::ExternalCircleBufferQueue>(
        std::span<std::byte>(reinterpret_cast<std::byte*>(m_RecvSlice.pData), m_RecvSlice.Length));

    m_pSendBuffer = std::make_unique<LibCommons::Buffers::ExternalCircleBufferQueue>(
        std::span<std::byte>(reinterpret_cast<std::byte*>(m_SendSlice.pData), m_SendSlice.Length));
}

RIOSession::~RIOSession() {}

bool RIOSession::Initialize()
{
    if (m_RQ == RIO_INVALID_RQ)
    {
        return false;
    }

    RequestRecv();

    return true;
}

void RIOSession::RequestRecv()
{
    if (m_bIsDisconnected)
    {
        return;
    }

    RIO_BUF buf{};
    buf.BufferId = m_RecvSlice.BufferId;
    buf.Offset = m_RecvSlice.Offset;
    buf.Length = m_RecvSlice.Length;

    Core::RioExtension::GetTable().RIOReceive(m_RQ, &buf, 1, 0, &m_RecvContext);
}

void RIOSession::SendMessage(const uint16_t packetId, const google::protobuf::Message& rfMessage)
{
    if (m_bIsDisconnected)
    {
        return;
    }

    const size_t bodySize = rfMessage.ByteSizeLong();
    const size_t totalSize = Core::Packet::GetHeaderSize() + Core::Packet::GetPacketIdSize() + bodySize;

    std::vector<std::span<std::byte>> buffers;
    if (!m_pSendBuffer->AllocateWrite(totalSize, buffers))
    {
        return;
    }

    uint16_t sizeNet = htons(static_cast<uint16_t>(totalSize));
    uint16_t idNet = htons(packetId);
    size_t writeOffset = 0;

    WriteToBuffers(buffers, writeOffset, &sizeNet, sizeof(sizeNet));
    WriteToBuffers(buffers, writeOffset, &idNet, sizeof(idNet));
    if (bodySize > 0)
    {
        std::string temp;
        rfMessage.SerializeToString(&temp);
        WriteToBuffers(buffers, writeOffset, temp.data(), temp.size());
    }

    TryPostSendFromQueue();
}

void RIOSession::WriteToBuffers(const std::vector<std::span<std::byte>>& buffers, size_t& offset, const void* pData, size_t len)
{
    const uint8_t* pSrc = reinterpret_cast<const uint8_t*>(pData);
    size_t remaining = len;
    for (const auto& span : buffers)
    {
        if (offset >= span.size())
        {
            offset -= span.size();
            continue;
        }

        size_t toCopy = (std::min)(remaining, span.size() - offset);
        std::memcpy(span.data() + offset, pSrc, toCopy);
        pSrc += toCopy;
        remaining -= toCopy;
        offset = 0;
        if (remaining == 0)
        {
            break;
        }
    }
}

void RIOSession::TryPostSendFromQueue()
{
    bool expected = false;
    if (!m_bSendInProgress.compare_exchange_strong(expected, true))
    {
        return;
    }

    std::vector<std::span<const std::byte>> readBuffers;
    size_t bytesToSend = m_pSendBuffer->GetReadBuffers(readBuffers);
    if (bytesToSend == 0)
    {
        m_bSendInProgress = false;
        return;
    }

    RIO_BUF buf{};
    buf.BufferId = m_SendSlice.BufferId;
    buf.Offset = m_SendSlice.Offset + static_cast<ULONG>(reinterpret_cast<const uint8_t*>(readBuffers[0].data()) - reinterpret_cast<const uint8_t*>(m_SendSlice.pData));
    buf.Length = static_cast<ULONG>(readBuffers[0].size());

    if (!Core::RioExtension::GetTable().RIOSend(m_RQ, &buf, 1, 0, &m_SendContext))
    {
        m_bSendInProgress = false;
    }
}

void RIOSession::OnRioIOCompleted(bool bSuccess, DWORD bytesTransferred, Core::RioOperationType opType)
{
    if (!bSuccess || (opType == Core::RioOperationType::Receive && bytesTransferred == 0))
    {
        m_bIsDisconnected = true;
        OnDisconnected();
        return;
    }

    switch (opType)
    {
    case Core::RioOperationType::Receive:
        m_pReceiveBuffer->CommitWrite(bytesTransferred);
        ReadReceivedBuffers();
        RequestRecv();
        break;
    case Core::RioOperationType::Send:
        m_pSendBuffer->Consume(bytesTransferred);
        m_bSendInProgress = false;
        TryPostSendFromQueue();
        break;
    }
}

void RIOSession::ReadReceivedBuffers()
{
    while (true)
    {
        auto frame = Core::PacketFramer::TryFrameFromBuffer(*m_pReceiveBuffer);
        if (frame.Result != Core::PacketFrameResult::Ok)
        {
            break;
        }

        if (frame.PacketOpt.has_value())
        {
            OnPacketReceived(*frame.PacketOpt);
        }
    }
}

} // namespace LibNetworks::Sessions