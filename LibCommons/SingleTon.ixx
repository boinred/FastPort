module;

#include <memory>
#include <mutex>
export module commons.singleton;


namespace LibCommons
{

export template<typename T>
class SingleTon
{

protected:
    SingleTon() = default;
    ~SingleTon() = default;

public:
    SingleTon(const SingleTon&) = delete;
    SingleTon& operator=(const SingleTon&) = delete;
    SingleTon(SingleTon&&) = delete;
    SingleTon& operator=(SingleTon&&) = delete;

    static T& GetInstance()
    {
        std::call_once(m_InstanceOnce,
            []() {
                m_instance.reset(new T());
            });
        return *m_instance;
    }



private:
    inline static std::unique_ptr<T> m_instance;
    inline static std::once_flag m_InstanceOnce;
};

} // namespace LibCommons