module;

#include <WinSock2.h>
#include <MSWSock.h>
#include <algorithm>
#include <cstring>
#include <vector>
#include <deque>
#include <mutex>
#include <span>
#include <spdlog/spdlog.h>

module networks.sessions.rio_session;

import commons.logger;

namespace LibNetworks::Sessions
{

RIOSession::RIOSession(const std::shared_ptr<Core::Socket>& pSocket, const Core::RioBufferSlice& recvSlice, const Core::RioBufferSlice& sendSlice, RIO_CQ completionQueue)
    : m_pSocket(pSocket), m_RecvSlice(recvSlice), m_SendSlice(sendSlice)
{
    m_RQ = Core::RioExtension::GetTable().RIOCreateRequestQueue(pSocket->GetSocket(), 1, 1, 1, 1, completionQueue, completionQueue, this);
    if (m_RQ == RIO_INVALID_RQ)
    {
        LibCommons::Logger::GetInstance().LogError("RIOSession", "RIOSession::Constructor - RIOCreateRequestQueue failed. Socket : {}, Error : {}", pSocket->GetSocket(), WSAGetLastError());

        // 
    }

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
        LibCommons::Logger::GetInstance().LogError("RIOSession", "RIOSession::Initialize - Failed to create RIO Request Queue. Session Id : {}", GetSessionId());
        return false;
    }

    RequestRecv();

    return true;
}

void RIOSession::RequestRecv()
{
    if (m_bIsDisconnected)
    {
        LibCommons::Logger::GetInstance().LogWarning("RIOSession", "RequestRecv called on disconnected session. Session Id : {}", GetSessionId());

        return;
    }

    std::vector<std::span<std::byte>> writeableBuffers;
    size_t freeSpace = m_pReceiveBuffer->GetWriteableBuffers(writeableBuffers);

    if (freeSpace == 0 || writeableBuffers.empty())
    {
        LibCommons::Logger::GetInstance().LogError("RIOSession", "RequestRecv - No free space in receive buffer. Session Id : {}", GetSessionId());
        return;
    }

    // RIOReceive는 한 번에 하나의 RIO_BUF(연속된 공간)를 받으므로, 
    // 원형 버퍼의 첫 번째 연속된 빈 공간만 사용합니다.
    RIO_BUF buf{};
    buf.BufferId = m_RecvSlice.BufferId;
    buf.Offset = m_RecvSlice.Offset + static_cast<ULONG>(writeableBuffers[0].data() - reinterpret_cast<std::byte*>(m_RecvSlice.pData));
    buf.Length = static_cast<ULONG>(writeableBuffers[0].size());

    LibCommons::Logger::GetInstance().LogDebug("RIOSession", "RequestRecv - RIOReceive called. Session Id : {}, Offset : {}, Length : {}", GetSessionId(), buf.Offset, buf.Length);

    if (!Core::RioExtension::GetTable().RIOReceive(m_RQ, &buf, 1, 0, &m_RecvContext))
    {
        LibCommons::Logger::GetInstance().LogError("RIOSession", "RequestRecv - RIOReceive failed. Session Id : {}, Error : {}", GetSessionId(), WSAGetLastError());
    }
}

void RIOSession::SendMessage(const uint16_t packetId, const google::protobuf::Message& rfMessage)
{
    if (m_bIsDisconnected)
    {
        return;
    }

    const size_t bodySize = rfMessage.ByteSizeLong();
    const size_t totalSize = Core::Packet::GetHeaderSize() + Core::Packet::GetPacketIdSize() + bodySize;

    // Safety Check: 대기 중인 데이터가 너무 많으면 연결 종료 (Backpressure)
    if (m_PendingTotalBytes.load(std::memory_order_relaxed) > MAX_PENDING_BYTES)
    {
        LibCommons::Logger::GetInstance().LogError("RIOSession", "SendMessage - Pending queue limit exceeded. Session Id : {}", GetSessionId());
        return;
    }

    // 패킷 직렬화
    std::vector<std::byte> packetData(totalSize);
    uint16_t sizeNet = htons(static_cast<uint16_t>(totalSize));
    uint16_t idNet = htons(packetId);

    std::memcpy(packetData.data(), &sizeNet, sizeof(sizeNet));
    std::memcpy(packetData.data() + sizeof(sizeNet), &idNet, sizeof(idNet));
    if (bodySize > 0)
    {
        rfMessage.SerializeToArray(packetData.data() + Core::Packet::GetHeaderSize() + Core::Packet::GetPacketIdSize(), static_cast<int>(bodySize));
    }

    {
        std::lock_guard lock(m_SendQueueMutex);

        bool bWrittenDirectly = false;

        // 1. Fast-Path: 큐가 비어있고, 버퍼에 즉시 쓸 공간이 충분한 경우
        if (m_PendingSendQueue.empty())
        {
            if (m_pSendBuffer->CanWriteSize() >= totalSize)
            {
                if (m_pSendBuffer->Write(packetData))
                {
                    bWrittenDirectly = true;
                }
            }
        }

        // 2. Slow-Path: 즉시 처리가 불가능하거나 큐에 데이터가 있는 경우 큐에 넣기
        if (!bWrittenDirectly)
        {
            m_PendingSendQueue.push_back({ std::move(packetData), 0 });
            m_PendingTotalBytes += totalSize;
        }
    }

    // 큐에 데이터가 있거나 방금 넣었으면 Flush 시도
    FlushPendingSendQueue();
}

void RIOSession::FlushPendingSendQueue()
{
    std::lock_guard lock(m_SendQueueMutex);

    while (!m_PendingSendQueue.empty())
    {
        auto& pending = m_PendingSendQueue.front();
        const size_t remainingInPacket = pending.Data.size() - pending.Offset;

        std::vector<std::span<std::byte>> writableBuffers;
        size_t availableSpace = m_pSendBuffer->GetWriteableBuffers(writableBuffers);

        if (availableSpace == 0)
            break;

        size_t toWrite = (std::min)(remainingInPacket, availableSpace);

        size_t written = 0;
        const uint8_t* pSrc = reinterpret_cast<const uint8_t*>(pending.Data.data() + pending.Offset);
        size_t remainingCopy = toWrite;

        for (auto& span : writableBuffers)
        {
            if (remainingCopy == 0) break;

            size_t copySize = (std::min)(remainingCopy, span.size());
            std::memcpy(span.data(), pSrc, copySize);

            pSrc += copySize;
            remainingCopy -= copySize;
            written += copySize;
        }

        m_pSendBuffer->CommitWrite(written);
        pending.Offset += written;

        if (pending.Offset >= pending.Data.size())
        {
            m_PendingTotalBytes -= pending.Data.size();
            m_PendingSendQueue.pop_front();
        }
        else
        {
            // 버퍼가 가득 참
            break;
        }
    }

    // 데이터가 준비되었으므로 전송 시도 (재귀 호출 아님, 내부 상태 플래그로 보호됨)
    TryPostSendFromQueue();
}

void RIOSession::WriteToBuffers(const std::vector<std::span<std::byte>>& buffers, size_t& offset, const void* pData, size_t len)
{
    const uint8_t* pSrc = reinterpret_cast<const uint8_t*>(pData);
    size_t remaining = len;
    size_t skip = offset;

    for (const auto& span : buffers)
    {
        if (skip >= span.size())
        {
            skip -= span.size();
            continue;
        }

        size_t toCopy = (std::min)(remaining, span.size() - skip);
        std::memcpy(span.data() + skip, pSrc, toCopy);
        pSrc += toCopy;
        remaining -= toCopy;
        skip = 0;
        if (remaining == 0)
        {
            break;
        }
    }
    offset += len;
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
        LibCommons::Logger::GetInstance().LogError("RIOSession", "TryPostSendFromQueue - RIOSend failed. Session Id : {}", GetSessionId());
    }
}

void RIOSession::OnRioIOCompleted(bool bSuccess, DWORD bytesTransferred, Core::RioOperationType opType)
{
    if (!bSuccess || (opType == Core::RioOperationType::Receive && bytesTransferred == 0))
    {
        LibCommons::Logger::GetInstance().LogInfo("RIOSession", "OnRioIOCompleted - Disconnected detected. Session Id : {}", GetSessionId());
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
        {
            std::lock_guard lock(m_SendQueueMutex);
            m_pSendBuffer->Consume(bytesTransferred);
            m_bSendInProgress = false;
        }
        FlushPendingSendQueue();
        break;
    }
}

void RIOSession::OnAccepted()
{
    LibCommons::Logger::GetInstance().LogInfo("RIOSession", "Session accepted. Session Id : {}", GetSessionId());

}

void RIOSession::OnConnected()
{
    LibCommons::Logger::GetInstance().LogInfo("RIOSession", "Session connected. Session Id : {}", GetSessionId());
}

void RIOSession::OnDisconnected()
{
    LibCommons::Logger::GetInstance().LogInfo("RIOSession", "Session disconnected. Session Id : {}", GetSessionId());
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