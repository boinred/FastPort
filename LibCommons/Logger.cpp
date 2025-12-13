module;
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/async.h>
#include <spdlog/spdlog.h>

module commons.logger;

namespace LibCommons
{

void Logger::Create(const std::string directoryName, const std::string fileName, const int maxFileSize, const int maxFileCount, const bool bServiceMode)
{
    m_bServiceMode = bServiceMode;

    // File existence check
    std::filesystem::path directoryPath = directoryName;

    if (!std::filesystem::exists(directoryPath))
    {
        if (!std::filesystem::create_directory(directoryPath))
        {
            std::cout << "Create directory failed." << directoryPath << std::endl;
        }
    }


    // 1. Initialize thread pool
    spdlog::init_thread_pool(8192, 1); // Queue size 8192, 1 thread

    // 2. Set pattern
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S] [%n] [%^%l%$] [thread %t] %v");

    m_FileName = std::format("{}/{}", directoryName, fileName);


    const auto C_MAX_FILE_SIZE = 1024 * 1024 * 10;   // 10MB
    // 2. Set sink
    m_pCreatedSilk = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(m_FileName, C_MAX_FILE_SIZE, 5, false);

    if (!m_bServiceMode)
    {
        std::shared_ptr<spdlog::logger> pConsoleLogger = spdlog::stderr_color_mt("console", spdlog::color_mode::automatic);
        pConsoleLogger->set_level(spdlog::level::debug);

        m_pConsoleLogger = pConsoleLogger;
    }
}

void Logger::Shutdown()
{
    spdlog::apply_all([&](auto pLogger) { pLogger->info("end of files"); });

    spdlog::shutdown();
}

void Logger::AddCategory(const std::string& categoryName)
{
    bool bFound = false;

    if (ReadLockBlock(m_Lock))
    {
        const auto iter = m_Categories.find(categoryName);
        if (iter != m_Categories.end())
        {
            bFound = true;
        }
    }

    if (bFound)
    {
        return;
    }

    if (WriteLockBlock(m_Lock))
    {
        auto insertResult = m_Categories.insert(categoryName);
        bFound = !insertResult.second;
    }

    if (bFound)
    {
        return;
    }

    // Create asynchronous logger
    auto pLogger = std::make_shared<spdlog::async_logger>(categoryName, m_pCreatedSilk, spdlog::thread_pool(), spdlog::async_overflow_policy::block);
#if _DEBUG
    pLogger->set_level(spdlog::level::debug);
#else 
    pLogger->set_level(spdlog::level::info);
#endif // #if _DEBUG

    spdlog::register_logger(pLogger);
}


} // namespace LibCommons