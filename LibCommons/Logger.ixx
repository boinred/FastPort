module;

#include <memory>

#include <spdlog/logger.h>
#include <spdlog/common.h>

export module commons.logger;

import std;
import commons.rwlock;

export namespace LibCommons
{
class Logger
{
public:
    void Create(const std::string directoryName, const std::string fileName, const int maxFileSize, const int maxFileCount, const bool bServiceMode = false);

    void Shutdown();

private:
    void AddCategory(const std::string& categoryName);

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