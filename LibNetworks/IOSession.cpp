module;

#include <WinSock2.h>
#include <spdlog/spdlog.h>

module networks.sessions.io_session;

import std; 

import commons.logger;
import networks.core.packet;
import networks.core.packet_framer;


namespace LibNetworks::Sessions
{

IOSession::IOSession(const std::shared_ptr<Core::Socket>& pSocket,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pReceiveBuffer,
    std::unique_ptr<LibCommons::Buffers::IBuffer> pSendBuffer)
    : m_pReceiveBuffer(std::move(pReceiveBuffer)),
    m_pSendBuffer(std::move(pSendBuffer)),
    m_pSocket(std::move(pSocket))
{
    // Recv는 고정 크기 버퍼를 재사용.
    m_RecvOverlapped.Buffers.resize(16 * 1024);
}

void IOSession::SendBuffer(std::span<const std::byte> data)
{
    if (data.empty() || !m_pSendBuffer)
    {
        LibCommons::Logger::GetInstance().LogError("IOSession", "SendBuffer() Invalid parameters. Session Id : {}", GetSessionId());

        return;
    }

    if (!m_pSendBuffer->Write(data))
    {
        LibCommons::Logger::GetInstance().LogError("IOSession", "SendBuffer() Failed to write data to send buffer. Session Id : {}, Data Length : {}", GetSessionId(), data.size());

        return;
    }

    TryPostSendFromQueue();
}

// 헬퍼 함수: 버퍼 조각들에 데이터를 씁니다.
static void WriteToBuffers(const std::vector<std::span<std::byte>>& buffers, size_t& bufferIdx, size_t& offsetInSpan, const void* data, size_t size)
{
    const unsigned char* src = static_cast<const unsigned char*>(data);
    size_t remaining = size;

    while (remaining > 0 && bufferIdx < buffers.size())
    {
        std::span<std::byte> span = buffers[bufferIdx];
        size_t spaceInSpan = span.size() - offsetInSpan;
        size_t toCopy = remaining < spaceInSpan ? remaining : spaceInSpan;
        

        std::memcpy(span.data() + offsetInSpan, src, toCopy);

        src += toCopy;
        remaining -= toCopy;
        offsetInSpan += toCopy;

        if (offsetInSpan == span.size())
        {
            bufferIdx++;
            offsetInSpan = 0;
        }
    }
}

void IOSession::SendMessage(const uint16_t packetId, const google::protobuf::Message& rfMessage)
{
    const size_t bodySize = rfMessage.ByteSizeLong();
    const size_t totalSize = Core::Packet::GetHeaderSize() + Core::Packet::GetPacketIdSize() + bodySize;

    if (!m_pSendBuffer)
    {
        return;
    }

    std::vector<std::span<std::byte>> buffers;
    // 링버퍼에 직접 공간 예약 (실패 시 전송 불가)
    if (!m_pSendBuffer->AllocateWrite(totalSize, buffers))
    {
        LibCommons::Logger::GetInstance().LogError("IOSession", "SendMessage() Send buffer overflow. Session Id : {}, Packet Size : {}", GetSessionId(), totalSize);
        RequestDisconnect();
        return;
    }

    // 헤더 및 패킷 ID 직렬화 (네트워크 바이트 오더)
    uint16_t sizeNet = htons(static_cast<uint16_t>(totalSize));
    uint16_t idNet = htons(packetId);

    size_t bufferIdx = 0;
    size_t offsetInSpan = 0;

    WriteToBuffers(buffers, bufferIdx, offsetInSpan, &sizeNet, sizeof(sizeNet));
    WriteToBuffers(buffers, bufferIdx, offsetInSpan, &idNet, sizeof(idNet));

    // Protobuf Body 직렬화
    if (bodySize > 0)
    {
        // 최적화: 버퍼가 1개이고 공간이 충분하면 SerializeToArray 사용 (가장 빠름)
        if (bufferIdx < buffers.size() && buffers[bufferIdx].size() - offsetInSpan >= bodySize)
        {
            rfMessage.SerializeToArray(buffers[bufferIdx].data() + offsetInSpan, static_cast<int>(bodySize));
        }
        else
        {
            // 버퍼가 쪼개져 있거나(Wrap around), 공간이 부족한 경우 (여기선 공간 부족일 리는 없음, Allocate 성공했으므로)
            // 임시 버퍼 없이 쪼개서 넣거나, Protobuf의 ArrayOutputStream을 여러 개 써야 함.
            // 간단하게: 임시 버퍼에 직렬화 후 WriteToBuffers (To avoid implementing complex ZeroCopyOutputStream right now)
            // *하지만* 우리는 "할당 최소화"가 목표이므로, 스택 메모리나 thread_local 버퍼를 활용해야 함.
            // 여기서는 일단 std::string을 쓰는 기존 방식보다는 낫지만, 완벽한 Zero-Copy는 아님 (Wrap around 시).
            // 그러나 대부분 패킷은 1개 버퍼에 들어갈 것임.
            // Wrap around 발생하는 경우에만 임시 복사 발생.
            
            // TODO: 완벽한 Zero-Copy를 위해 RingBufferOutputStream 구현 필요.
            // 현재는 간단히 처리.
            std::string temp; // Fallback
            rfMessage.SerializeToString(&temp);
            WriteToBuffers(buffers, bufferIdx, offsetInSpan, temp.data(), temp.size());
        }
    }

    TryPostSendFromQueue();
}

void IOSession::RequestReceived()
{
    if (!PostZeroByteRecv())
    {
        RequestDisconnect();
    }
}

bool IOSession::PostZeroByteRecv()
{
    // 이미 진행 중인지 확인
    bool expected = false;
    if (!m_RecvInProgress.compare_exchange_strong(expected, true))
    {
        return true;
    }

    m_RecvOverlapped.ResetOverlapped();
    m_RecvOverlapped.IsZeroByte = true; // 플래그 설정
    m_RecvOverlapped.WSABufs.clear();

    WSABUF wsaBuf{};
    wsaBuf.buf = nullptr;
    wsaBuf.len = 0;
    
    // WSARecv (Flags=0, Buffer=0, Len=0)
    DWORD flags = 0;
    DWORD bytes = 0;

    int result = ::WSARecv(m_pSocket->GetSocket(), 
        &wsaBuf, 
        1, 
        &bytes, 
        &flags, 
        &m_RecvOverlapped.Overlapped, 
        nullptr);

    if (result == SOCKET_ERROR)
    {
        int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            LibCommons::Logger::GetInstance().LogError("IOSession", "PostZeroByteRecv() WSARecv failed. Session Id : {}, Error Code : {}", GetSessionId(), err);

            m_RecvInProgress.store(false);
            return false;
        }
    }

    return true;
}

bool IOSession::PostRealRecv()
{
    // PostRealRecv는 OnIOCompleted(ZeroByte)에서 호출되므로
    // 이미 m_RecvInProgress는 true 상태임 (PostZeroByteRecv가 걸어둠).
    // 만약 false라면 뭔가 잘못된 상태지만, 재진입 방지는 이미 됨.
    // *주의*: OnIOCompleted에서 m_RecvInProgress를 false로 만들지 않고 RealRecv를 걸 수도 있고
    // false로 만들고 다시 걸 수도 있음. 여기서는 false로 만들고 진입한다고 가정하고 다시 체크?
    // 아니면 그냥 진행? -> 일관성을 위해 OnIOCompleted에서 atomic을 유지한 채로 넘어오는게 좋음.
    // 하지만 구조상 OnIOCompleted 진입 시점에 이미 완료된 상태이므로 false로 두는게 맞을 수도 있음.
    // *수정*: OnIOCompleted에서 false로 셋팅하고 여기 호출한다고 가정.
    
    // 1. Zero-Copy Recv: 수신 버퍼의 쓰기 가능한 공간을 직접 WSABUF로 설정
    std::vector<std::span<std::byte>> buffers;
    
    // 버퍼가 꽉 찼는지 확인
    size_t writableSize = 0;
    if (m_pReceiveBuffer)
    {
         writableSize = m_pReceiveBuffer->GetWriteableBuffers(buffers);
    }

    if (writableSize == 0)
    {
         LibCommons::Logger::GetInstance().LogError("IOSession", "PostRealRecv() Receive buffer full. Session Id : {}", GetSessionId());
         RequestDisconnect();
         return false;
    }

    m_RecvOverlapped.ResetOverlapped();
    m_RecvOverlapped.IsZeroByte = false; // Real Recv
    m_RecvOverlapped.WSABufs.clear();
    m_RecvOverlapped.WSABufs.reserve(buffers.size());
    for (const auto& span : buffers)
    {
        WSABUF wsaBuf{};
        wsaBuf.buf = reinterpret_cast<char*>(span.data());
        wsaBuf.len = static_cast<ULONG>(span.size());
        m_RecvOverlapped.WSABufs.push_back(wsaBuf);
    }
    
    DWORD flags = 0;
    DWORD bytes = 0;

    int result = ::WSARecv(m_pSocket->GetSocket(), 
        m_RecvOverlapped.WSABufs.data(), 
        static_cast<DWORD>(m_RecvOverlapped.WSABufs.size()), 
        &bytes, 
        &flags, 
        &m_RecvOverlapped.Overlapped, 
        nullptr);

    if (result == SOCKET_ERROR)
    {
        int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            LibCommons::Logger::GetInstance().LogError("IOSession", "PostRealRecv() WSARecv failed. Session Id : {}, Error Code : {}", GetSessionId(), err);
            
            // 여기서 실패하면(연결 끊김 등), 외부에서 atomic 처리를 해제해줘야 함.
            // (호출자가 false로 만들고 불렀으면 상관없음)
            return false;
        }
    }

    return true;
}

bool IOSession::TryPostSendFromQueue()
{
    // Send는 outstanding 1개만 유지
    // 1. 다른 스레드가 이미 전송 작업을 시작했는지 확인 (진입 잠금)
    bool expected = false;
    if (!m_SendInProgress.compare_exchange_strong(expected, true))
    {
        // 1. 다른 스레드가 이미 전송 작업을 시작했는지 확인 (진입 잠금)
        return true;
    }

    if (!m_pSendBuffer)
    {
        m_SendInProgress.store(false);
        return true;
    }

    // 링버퍼에서 전송할 데이터 포인터들을 직접 가져옵니다 (Zero-Copy)
    // 데이터는 전송이 완료된 후(OnIOCompleted)에 Consume합니다.
    std::vector<std::span<const std::byte>> buffers;
    const size_t bytesToSend = m_pSendBuffer->GetReadBuffers(buffers);

    if (bytesToSend == 0)
    {
        m_SendInProgress.store(false);
        return true;
    }

    m_SendOverlapped.RequestedBytes = bytesToSend;
    m_SendOverlapped.ResetOverlapped();
    
    // WSABUF 배열 구성
    m_SendOverlapped.WSABufs.clear();
    m_SendOverlapped.WSABufs.reserve(buffers.size());
    for (const auto& span : buffers)
    {
        WSABUF wsaBuf{};
        wsaBuf.buf = const_cast<char*>(reinterpret_cast<const char*>(span.data()));
        wsaBuf.len = static_cast<ULONG>(span.size());
        m_SendOverlapped.WSABufs.push_back(wsaBuf);
    }

    DWORD bytesSent = 0;
    int result = ::WSASend(m_pSocket->GetSocket(), 
        m_SendOverlapped.WSABufs.data(), 
        static_cast<DWORD>(m_SendOverlapped.WSABufs.size()), 
        &bytesSent, 
        0, 
        &m_SendOverlapped.Overlapped, 
        nullptr);

    if (result == SOCKET_ERROR)
    {
        int err = ::WSAGetLastError();
        if (err != WSA_IO_PENDING)
        {
            LibCommons::Logger::GetInstance().LogError("IOSession", "TryPostSendFromQueue() WSASend failed. Session Id : {}, Error Code : {}", GetSessionId(), err);

            m_SendInProgress.store(false);
            return false;
        }
    }

    // 성공 또는 Pending 시:
    // Consume은 여기서 하지 않고, OnIOCompleted 시점에 처리합니다.
    // 기존의 m_pSendBuffer->Consume(bytesToSend); 제거됨.

    return true;
}

void IOSession::OnIOCompleted(bool bSuccess, DWORD bytesTransferred, OVERLAPPED* pOverlapped)
{
    if (!pOverlapped)
    {
        return;
    }

    // 멤버 Overlapped 주소로 구분
    if (pOverlapped == &(m_RecvOverlapped.Overlapped))
    {
        // atomic flag 잠시 해제 (다음 요청을 위해)
        // 하지만 여기서 해제하면 PostRealRecv 시 경합? -> PostRealRecv 안에서 다시 true 만듦?
        // 아니면 true 상태 유지하고, 다 끝나면 false?
        // "ZeroByte 완료" -> m_RecvInProgress는 여전히 내꺼임.
        // "RealRecv 완료" -> m_RecvInProgress 내꺼임.
        // 따라서 여기서 false로 만들지 말고, 흐름에 따라 결정.

        if (!bSuccess)
        {
            m_RecvInProgress.store(false);
            LibCommons::Logger::GetInstance().LogError("IOSession", "OnIOCompleted() Recv failed. Session Id : {}, Error Code : {}", GetSessionId(), GetLastError());
            RequestDisconnect();
            return;
        }

        // 1. Zero-byte Recv 완료 처리
        if (m_RecvOverlapped.IsZeroByte)
        {
            if (bytesTransferred == 0)
            {
                // Zero-byte Recv 완료 (데이터 도착 알림)
                
                // 실제 데이터를 읽기 위해 Non-Blocking Recv (or Real Recv) 수행
                // 여기서는 PostRealRecv 호출. 이 함수는 실패 시 m_RecvInProgress = false 처리 해야 함.
                // 편의상 여기서 atomic flag를 일단 false로 하고 다시 try?
                // 아니면 그대로 pass? -> PostRealRecv 내부에서 check 안하고 바로 걸면 됨.
                // PostRealRecv 수정이 필요함 (atomic check 제거 버전 필요하거나...)
                // 위에서 수정한 PostRealRecv는 atomic check 없음. 
                // 단, 외부에서 호출하는 경우(RequestReceived)와 겹치지 않게 주의.
                // RequestReceived는 PostZeroByteRecv를 호출함.
                
                // 아무튼 여기서 바로 Real Recv 건다.
                if (!PostRealRecv())
                {
                    m_RecvInProgress.store(false);
                    RequestDisconnect();
                }
                return;
            }
            else
            {
                // Zero-byte 요청했는데 bytes > 0 일 수는 없음 (버퍼크기 0이라서).
                // 혹시 모르니 종료 처리
                m_RecvInProgress.store(false);
                return;
            }
        }
        else // 2. Real Recv 완료 처리
        {
            if (bytesTransferred == 0)
            {
                // 실제 Recv 걸었는데 0이면 연결 종료
                m_RecvInProgress.store(false);
                LibCommons::Logger::GetInstance().LogInfo("IOSession", "OnIOCompleted() Recv 0 byte (Real). Disconnected. Session Id : {}", GetSessionId());
                RequestDisconnect();
                return;
            }

            // Zero-Copy Recv Commit
            if (!m_pReceiveBuffer->CommitWrite(bytesTransferred))
            {
                 LibCommons::Logger::GetInstance().LogError("IOSession", "OnIOCompleted() CommitWrite failed (Overflow?). Session Id : {}, Bytes : {}", GetSessionId(), bytesTransferred);
                 m_RecvInProgress.store(false);
                 RequestDisconnect();
                 return;
            }

            ReadReceivedBuffers();
            
            // 다 처리했으면 다시 Zero-byte Recv 걸기
            // 여기서 m_RecvInProgress를 false로 하고 RequestReceived() 호출?
            // 아니면 그냥 PostZeroByteRecv?
            m_RecvInProgress.store(false);
            RequestReceived();
            
            return;
        }
    }

    if (pOverlapped == &(m_SendOverlapped.Overlapped))
    {
        m_SendInProgress.store(false);

        if (!bSuccess)
        {
            LibCommons::Logger::GetInstance().LogError("IOSession", "OnIOCompleted() Send failed. Session Id : {}, Error Code : {}", GetSessionId(), GetLastError());
            RequestDisconnect();
            return;
        }

        const size_t requested = m_SendOverlapped.RequestedBytes;
        
        // 전송 완료된 만큼 버퍼 비우기 (Delayed Consume)
        if (m_pSendBuffer)
        {
            m_pSendBuffer->Consume(bytesTransferred);
        }

        if (requested > 0 && bytesTransferred < requested)
        {
             // TODO: 부분 전송 처리. 
             // 현재 로직상으로는 다음 TryPostSendFromQueue에서 남은 데이터를 다시 시도함.
            LibCommons::Logger::GetInstance().LogDebug("IOSession", "OnIOCompleted() Partial send. Session Id : {}, Requested : {}, Transferred : {}", GetSessionId(), requested, bytesTransferred);
        }

        OnSent(bytesTransferred);

        const bool hasPending = m_pSendBuffer && (m_pSendBuffer->CanReadSize() > 0);

        if (hasPending)
        {
            TryPostSendFromQueue();
        }

        return;
    }
}

void IOSession::ReadReceivedBuffers()
{
    if (!m_pReceiveBuffer)
    {
        return;
    }

    auto& logger = LibCommons::Logger::GetInstance();

    while (true)
    {
        auto frame = Core::PacketFramer::TryFrameFromBuffer(*m_pReceiveBuffer);
        if (frame.Result == Core::PacketFrameResult::NeedMore)
        {
            break;
        }

        if (frame.Result == Core::PacketFrameResult::Invalid)
        {
            logger.LogError("IOSession", "ReadReceivedBuffers() Invalid packet frame. Session Id : {}", GetSessionId());
            RequestDisconnect();
            break;
        }

        if (!frame.PacketOpt.has_value())
        {
            logger.LogError("IOSession", "ReadReceivedBuffers() Packet frame ok but packet missing. Session Id : {}", GetSessionId());
            RequestDisconnect();
            break;
        }

        OnPacketReceived(*frame.PacketOpt);
    }
}

void IOSession::RequestDisconnect()
{
    auto& logger = LibCommons::Logger::GetInstance();

    bool expected = false;
    if (!m_DisconnectRequested.compare_exchange_strong(expected, true))
    {
        return;
    }

    if (!m_pSocket)
    {
        logger.LogWarning("IOSession", "RequestDisconnect() Socket is null. Session Id : {}", GetSessionId());
        OnDisconnected();
        return;
    }

    // 강제 종료: send/recv 중단 + 즉시 close
    m_pSocket->Shutdown(SD_BOTH);
    m_pSocket->Close();

    m_RecvInProgress.store(false);
    m_SendInProgress.store(false);

    if (m_pReceiveBuffer)
    {
        m_pReceiveBuffer->Clear();
    }

    if (m_pSendBuffer)
    {
        m_pSendBuffer->Clear();
    }

    logger.LogInfo("IOSession", "RequestDisconnect() Socket closed. Session Id : {}", GetSessionId());

    OnDisconnected();
}



} // namespace LibNetworks::Sessions