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
