module; 

#include <windows.h>
#include <memory>

export module commons.rwlock;

export namespace LibCommons
{

class RWLock
{
public:
	RWLock();

	void ReadLock();
	void ReadUnLock();

	void WriteLock();
	void WriteUnLock();

private:

	SRWLOCK m_SRWLock{};
};

class ReferenceReadLockBlock
{
public:
	ReferenceReadLockBlock(RWLock& rfRWLock);
	~ReferenceReadLockBlock();

	ReferenceReadLockBlock(const ReferenceReadLockBlock&) = delete;
	ReferenceReadLockBlock& operator=(const ReferenceReadLockBlock&) = delete;

	[[nodiscard]] operator bool() noexcept { return true; }
private:

	RWLock& m_rfRWLock;
};

class ReferenceWriteLockBlock
{
public:
	ReferenceWriteLockBlock(RWLock& rfRWLock);
	~ReferenceWriteLockBlock();

	ReferenceWriteLockBlock(const ReferenceWriteLockBlock&) = delete;
	ReferenceWriteLockBlock& operator=(const ReferenceWriteLockBlock&) = delete;

	[[nodiscard]] operator bool() noexcept { return true; }

private:
	RWLock& m_rfRWLock;
};

[[nodiscard]] inline ReferenceReadLockBlock ReadLockBlock(LibCommons::RWLock& rwLock)
{
	return ReferenceReadLockBlock(rwLock);
}
[[nodiscard]] inline ReferenceWriteLockBlock WriteLockBlock(LibCommons::RWLock& rwLock)
{
	return ReferenceWriteLockBlock(rwLock);
}


} // namespace LibCommons