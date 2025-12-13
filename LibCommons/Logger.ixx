module;

#include <memory>
#include <string>
#include <unordered_set>
#include <format>

#include <spdlog/logger.h>
#include <spdlog/common.h>
#include <spdlog/spdlog.h>


export module commons.logger;

import commons.rwlock;
import commons.singleton;

export namespace LibCommons
{
class Logger : public SingleTon<Logger>
{
private:
    friend class SingleTon<Logger>;
    Logger() = default;

public:
    void Create(const std::string directoryName, const std::string fileName, const int maxFileSize, const int maxFileCount, const bool bServiceMode = false);

    void Shutdown();

    template<typename ... Args>
    void LogDebug(const std::string categaryName, spdlog::string_view_t fmt, Args&&... args)
    {
        Log(categaryName, spdlog::level::level_enum::debug, fmt, args...);
    }

    template<typename ... Args>
    void LogWarning(const std::string categaryName, spdlog::string_view_t fmt, Args&&... args)
    {
        Log(categaryName, spdlog::level::level_enum::warn, fmt, args...);
    }

    template<typename ... Args>
    void LogInfo(const std::string categaryName, spdlog::string_view_t fmt, Args &&...args)
    {
        Log(categaryName, spdlog::level::level_enum::info, fmt, args...);
    }

    template<typename ... Args>
    void LogError(const std::string categaryName, spdlog::string_view_t fmt, Args&&... args)
    {
        Log(categaryName, spdlog::level::level_enum::err, fmt, args...);
    }

    template<typename ... Args>
    void LogCritical(const std::string categaryName, spdlog::string_view_t fmt, Args&&... args)
    {
        Log(categaryName, spdlog::level::level_enum::critical, fmt, args...);
    }

protected:
    template<typename ... Args>
    void Log(const std::string categaryName, spdlog::level::level_enum lvl, spdlog::string_view_t fmt, Args&&... args)
    {
        auto pLogger = spdlog::get(categaryName);
        if (!pLogger)
        {
            AddCategory(categaryName);

            pLogger = spdlog::get(categaryName);
        }

        if (pLogger)
        {

            pLogger->log(lvl, fmt, std::forward<Args>(args)...);
        }

        auto pConsoleLogger = m_pConsoleLogger;
        if (pConsoleLogger)
        {
            pConsoleLogger->log(lvl, fmt, std::forward<Args>(args)...);
        }
    }

private:
    void AddCategory(const std::string& categoryName);

private:
    RWLock m_Lock;

    bool m_bServiceMode = false;

    int m_MaxFileSize = 0;
    int m_MaxFileCount = 0;

    std::string m_FileName;

    std::shared_ptr<spdlog::sinks::sink> m_pCreatedSilk;

    std::unordered_set<std::string> m_Categories;

    std::shared_ptr<spdlog::logger> m_pConsoleLogger;
};

} // namespace LibCommons
