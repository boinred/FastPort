module;

#include <windows.h>

module commons.rwlock;

namespace LibCommons
{

RWLock::RWLock()
{
    ::InitializeSRWLock(&m_SRWLock);
}

void RWLock::ReadLock()
{
    ::AcquireSRWLockShared(&m_SRWLock);
}

void RWLock::ReadUnLock()
{
    ::ReleaseSRWLockShared(&m_SRWLock);
}

void RWLock::WriteLock()
{
    ::AcquireSRWLockExclusive(&m_SRWLock);
}

void RWLock::WriteUnLock()
{
    ::ReleaseSRWLockExclusive(&m_SRWLock);
}

ReferenceReadLockBlock::ReferenceReadLockBlock(RWLock& rfRWLock) : m_rfRWLock(rfRWLock)
{
    m_rfRWLock.ReadLock();
}

ReferenceReadLockBlock::~ReferenceReadLockBlock()
{
    m_rfRWLock.ReadUnLock();
}

ReferenceWriteLockBlock::ReferenceWriteLockBlock(RWLock& rfRWLock) :m_rfRWLock(rfRWLock)
{
    m_rfRWLock.WriteLock();
}

ReferenceWriteLockBlock::~ReferenceWriteLockBlock()
{
    m_rfRWLock.WriteUnLock();
}




} // namespace LibCommons


