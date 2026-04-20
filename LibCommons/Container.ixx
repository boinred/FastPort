module;

#include <unordered_map>
#include <memory>
#include <utility>
#include <type_traits>

export module commons.container;

import std;
import commons.rwlock;

namespace LibCommons
{

export template<typename Key, typename T>
class Container
{
public:
    using key_type = Key;
    using mapped_type = T;

    Container() = default;
    ~Container() = default;

    Container(const Container&) = delete;
    Container& operator=(const Container&) = delete;

    bool Add(const Key& key, T value)
    {
        auto lock = WriteLockBlock(m_Lock);
        auto [it, inserted] = m_Storage.emplace(key, std::move(value));
        return inserted;
    }

    template<typename... Args>
    bool Emplace(const Key& key, Args&&... args)
    {
        auto lock = WriteLockBlock(m_Lock);
        auto [it, inserted] = m_Storage.emplace(key, T(std::forward<Args>(args)...));
        return inserted;
    }

    template<typename Predicate>
    T* FindIf(Predicate&& predicate)
    {
        auto lock = ReadLockBlock(m_Lock);
        for (auto& [k, v] : m_Storage)
        {
            if (std::invoke(predicate, k, v))
            {
                return &v;
            }
        }
        return nullptr;
    }

    template<typename Predicate>
    const T* FindIf(Predicate&& predicate) const
    {
        auto lock = ReadLockBlock(m_Lock);
        for (auto& [k, v] : m_Storage)
        {
            if (std::invoke(predicate, k, v))
            {
                return &v;
            }
        }
        return nullptr;
    }

    bool Remove(const Key& key)
    {
        auto lock = WriteLockBlock(m_Lock);
        return m_Storage.erase(key) > 0;
    }

    // Design Ref: session-idle-timeout §4.1 — read-lock 을 유지한 채 각 엔트리를 콜백에 전달.
    // 콜백 내부에서 Container 의 Add/Remove/Clear 호출은 write 재진입이 되어 데드락 유발하므로 금지.
    // 외부 상태(로컬 벡터 등) 에만 기록하거나 스냅샷 수집 용도로 사용.
    template<typename Fn>
    void ForEach(Fn&& fn) const
    {
        auto lock = ReadLockBlock(m_Lock);
        for (auto const& [k, v] : m_Storage)
        {
            fn(k, v);
        }
    }

    // Design Ref: session-idle-timeout §4.1 — 값 복사로 스냅샷 반환.
    // 호출자는 락 제약 없이 결과 vector 를 자유롭게 처리 가능. 큰 컨테이너는 복사 비용 주의.
    std::vector<std::pair<Key, T>> Snapshot() const
    {
        auto lock = ReadLockBlock(m_Lock);
        std::vector<std::pair<Key, T>> result;
        result.reserve(m_Storage.size());
        for (auto const& [k, v] : m_Storage)
        {
            result.emplace_back(k, v);
        }
        return result;
    }

    template<typename Predicate>
    size_t RemoveIf(Predicate&& predicate)
    {
        auto lock = WriteLockBlock(m_Lock);
        size_t removed = 0;
        for (auto it = m_Storage.begin(); it != m_Storage.end();)
        {
            if (std::invoke(predicate, it->first, it->second))
            {
                it = m_Storage.erase(it);
                ++removed;
            }
            else
            {
                ++it;
            }
        }
        return removed;
    }

    size_t Size() const
    {
        auto lock = ReadLockBlock(m_Lock);
        return m_Storage.size();
    }

    void Clear()
    {
        auto lock = WriteLockBlock(m_Lock);
        m_Storage.clear();
    }

private:
    std::unordered_map<Key, T> m_Storage;
    mutable RWLock m_Lock;
};

} // namespace LibCommons
