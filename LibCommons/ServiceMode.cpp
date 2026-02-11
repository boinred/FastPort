module;

#include <cxxopts.hpp>
#include <Windows.h>
#include <assert.h>
#include <iostream>
#include <thread>
#include <spdlog/spdlog.h>
module commons.service_mode;

import commons.strconverter;
import commons.logger;


namespace LibCommons
{

std::shared_ptr<ServiceMode> ServiceMode::m_pServiceMode;


bool ServiceMode::Installer::Install(LPCWSTR ServiceName, LPCWSTR DisplayName, DWORD dwStartType, LPCWSTR Dependencies, LPCWSTR Account, LPCWSTR password)
{
    auto& logger = LibCommons::Logger::GetInstance();

    // Open the local default service control manager database.
    SC_HANDLE hManager = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    assert(nullptr != hManager);
    if (nullptr == hManager)
    {
        std::cout << "SCMManager open failed. : " << ::GetLastError() << std::endl;

        //spdlog::info("Hello, {}!", "world");
        logger.LogError("ServiceMode", "Install, SCManager open is failed. Error : {}", ::GetLastError());

        return false;
    }

    TCHAR moduleFileName[MAX_PATH] = {};
    ::GetModuleFileName(nullptr, moduleFileName, MAX_PATH);

    SC_HANDLE hService = ::CreateService(
        hManager,					// SCManager database
        ServiceName,				// Name of service
        DisplayName,				// Name to display 
        SERVICE_QUERY_STATUS,		// Desired access
        SERVICE_WIN32_OWN_PROCESS,	// Service type
        SERVICE_AUTO_START,		// Service start type
        SERVICE_ERROR_NORMAL,		// Error control type
        moduleFileName,				// Service`s binary
        nullptr,					// No load ordering group
        nullptr,					// No tag identifier
        nullptr,				// Dependencies 
        nullptr,					// Service running account
        nullptr);					// Password of the account

    if (nullptr == hService)
    {
        logger.LogError(
            "ServiceMode",
            "Service is install failed. Service Name : {}, Display Name : {}, Path : {}, Error : {}",
            StrConverter::ToAnsi(ServiceName),
            StrConverter::ToAnsi(DisplayName),
            StrConverter::ToAnsi(moduleFileName),
            ::GetLastError());

        std::cout << "To service install is not success. : " << ::GetLastError() << std::endl;
        ::CloseServiceHandle(hManager);

        return false;
    }

    logger.LogInfo(
        "ServiceMode",
        "Service is install success. Service Name : {}, Display Name : {}, Path : {}",
        StrConverter::ToAnsi(ServiceName),
        StrConverter::ToAnsi(DisplayName),
        StrConverter::ToAnsi(moduleFileName));

    ::CloseServiceHandle(hManager);
    ::CloseServiceHandle(hService);


    return true;
}

bool ServiceMode::Installer::Stop(LPCWSTR ServiceName)
{
    auto& logger = LibCommons::Logger::GetInstance();

    ULONG64 dwStartTime = ::GetTickCount64();
    DWORD dwBytesNeeded = 0;

    // Get a handle to the SCM database. 
    SC_HANDLE hManager = OpenSCManager(
        nullptr,                    // local computer
        nullptr,                    // ServicesActive database 
        SC_MANAGER_ALL_ACCESS);  // full access rights 

    if (nullptr == hManager)
    {
        logger.LogError("ServiceMode", "Stop, SCManager open is failed. Error : {}", ::GetLastError());
        return false;
    }

    // Get a handle to the service.

    SC_HANDLE hService = OpenService(
        hManager,         // SCM database 
        ServiceName,            // name of service 
        SERVICE_STOP |
        SERVICE_QUERY_STATUS |
        SERVICE_ENUMERATE_DEPENDENTS);

    if (hService == nullptr)
    {
        logger.LogError("ServiceMode", "Stop, Service is not stopped. Service Name : {}, Error : {}", StrConverter::ToAnsi(ServiceName), ::GetLastError());

        ::CloseServiceHandle(hManager);

        return false;
    }

    DWORD dwTimeout = 30000; // 30-second time-out
    bool bPending = false;
    DWORD dwWaitTime = 0;
    SERVICE_STATUS_PROCESS ssp{};
    do
    {
        // Make sure the service is not already stopped.
        if (!QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(SERVICE_STATUS_PROCESS), &dwBytesNeeded))
        {
            logger.LogError("ServiceMode", "Stop, QueryServiceStatusEx. Service Name : {}, Error : {}", StrConverter::ToAnsi(ServiceName), ::GetLastError());

            ::CloseServiceHandle(hManager);
            ::CloseServiceHandle(hService);

            return false;
        }

        if (bPending)
        {
            dwWaitTime = ssp.dwWaitHint / 10;
            if (dwWaitTime < 1000)
                dwWaitTime = 1000;
            else if (dwWaitTime > 10000)
                dwWaitTime = 10000;

            Sleep(dwWaitTime);

            std::cout << "Service stop pending..." << std::endl;
        }

        if (ssp.dwCurrentState == SERVICE_STOPPED)
        {
            logger.LogError("ServiceMode", "Stop, Service is already stopped. Service Name : {}", StrConverter::ToAnsi(ServiceName));

            ::CloseServiceHandle(hManager);
            ::CloseServiceHandle(hService);

            return true;
        }

        if (bPending)
        {
            if (::GetTickCount64() - dwStartTime > dwTimeout)
            {
                logger.LogError("ServiceMode", "Stop, Service stop timed out. Service Name : {}", StrConverter::ToAnsi(ServiceName));
                ::CloseServiceHandle(hManager);
                ::CloseServiceHandle(hService);

                return false;
            }
        }

        if (!bPending) bPending = true;

    } while (ssp.dwCurrentState == SERVICE_STOP_PENDING);


    // If the service is running, dependencies must be stopped first.
    // StopDependentServices();

    // Send a stop code to the service.

    if (!ControlService(hService, SERVICE_CONTROL_STOP, (LPSERVICE_STATUS)&ssp))
    {
        logger.LogError(
            "ServiceMode",
            "Stop, ControlService is not success. Service Name : {}, Error : {}",
            StrConverter::ToAnsi(ServiceName),
            ::GetLastError());

        ::CloseServiceHandle(hManager);
        ::CloseServiceHandle(hService);

        return false;
    }

    // Wait for the service to stop.

    while (ssp.dwCurrentState != SERVICE_STOPPED)
    {
        Sleep(ssp.dwWaitHint);

        if (!QueryServiceStatusEx(hService, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(SERVICE_STATUS_PROCESS), &dwBytesNeeded))
        {
            logger.LogError(
                "ServiceMode",
                "Stop, QueryServiceStatusEx. Service Name : {}, Error : {}",
                StrConverter::ToAnsi(ServiceName),
                ::GetLastError());

            ::CloseServiceHandle(hManager);
            ::CloseServiceHandle(hService);

            return false;
        }

        if (ssp.dwCurrentState == SERVICE_STOPPED)
            break;

        if (GetTickCount64() - dwStartTime > dwTimeout)
        {
            logger.LogError(
                "ServiceMode",
                "Stop, Service stop timed out. Service Name : {}",
                StrConverter::ToAnsi(ServiceName));

            ::CloseServiceHandle(hManager);
            ::CloseServiceHandle(hService);

            return false;
        }
    }

    logger.LogInfo(
        "ServiceMode",
        "Stop, Service is stopped. Service Name : {}",
        StrConverter::ToAnsi(ServiceName));

    ::CloseServiceHandle(hManager);
    ::CloseServiceHandle(hService);
    return true;
}

bool ServiceMode::Installer::UnInstall(LPCWSTR ServiceName)
{
    auto& logger = LibCommons::Logger::GetInstance();
    SC_HANDLE hManager = ::OpenSCManager(nullptr, nullptr, SC_MANAGER_CONNECT);
    assert(nullptr != hManager);
    if (nullptr == hManager)
    {
        logger.LogError("ServiceMode", "UnInstall, SCManager open is failed. Error : {}", ::GetLastError());

        return false;
    }

    SC_HANDLE hService = ::OpenService(hManager, ServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (nullptr == hService)
    {
        logger.LogError("ServiceMode", "UnInstall, Service is not found. Service Name : {}", StrConverter::ToAnsi(ServiceName));

        return true;
    }

    // try to stop the service 
    SERVICE_STATUS ss = {};
    ::ControlService(hService, SERVICE_CONTROL_STOP, &ss);

    std::this_thread::sleep_for(std::chrono::seconds(1));

    int countOfTryTo = 0;
    while (!::QueryServiceStatus(hService, &ss))
    {
        if (ss.dwCurrentState == SERVICE_STOP_PENDING)
        {
            countOfTryTo++;
            logger.LogWarning("ServiceMode", "Service stop is pending, Service Name : {}", StrConverter::ToAnsi(ServiceName));

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        else
        {
            break;
        }
    }

    if (ss.dwCurrentState == SERVICE_STOPPED)
    {
        logger.LogInfo("ServiceMode", "Service is stopped, Service Name : {}", StrConverter::ToAnsi(ServiceName));
    }
    else
    {
        logger.LogError("ServiceMode", "Service is stopp failed, Service Name : {}", StrConverter::ToAnsi(ServiceName));
    }

    if (!::DeleteService(hService))
    {
        logger.LogError("ServiceMode", "Service is delete failed, Service Name : {}, Error Code : {}", StrConverter::ToAnsi(ServiceName), ::GetLastError());

        return false;
    }


    logger.LogInfo("ServiceMode", "Service is delete success. Service Name : {}", StrConverter::ToAnsi(ServiceName));

    return true;
}

ServiceMode::ServiceMode(const bool bCanStop /*= true*/, const bool bCanShutdown /*= true*/, const bool bCanPauseContinue /*= false*/)
{
    m_StatusHandle = nullptr;

    m_Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;

    // The service runs in its own process
    m_Status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;

    // The service is starting.
    m_Status.dwCurrentState = SERVICE_START_PENDING;

    // The accepted commands of the service.
    DWORD dwControlsAccepted = SERVICE_ACCEPT_SESSIONCHANGE;
    if (bCanStop)
    {
        dwControlsAccepted |= SERVICE_ACCEPT_STOP;
    }
    if (bCanShutdown)
    {
        dwControlsAccepted |= SERVICE_ACCEPT_SHUTDOWN;
    }

    if (bCanPauseContinue)
    {
        dwControlsAccepted |= SERVICE_ACCEPT_PAUSE_CONTINUE;
    }

    m_Status.dwControlsAccepted = dwControlsAccepted;

    m_Status.dwWin32ExitCode = NO_ERROR;
    m_Status.dwServiceSpecificExitCode = 0;
    m_Status.dwCheckPoint = 0;
    m_Status.dwWaitHint = 0;
}


bool ServiceMode::Execute(const DWORD argc, const char* argv[])
{
    auto& logger = LibCommons::Logger::GetInstance();

    cxxopts::Options options(m_ServiceName, "Allowed options.");

    options.add_options()
        (C_HELP, std::format("help message for {}", m_ServiceName))
        (C_INSTALL, "service is install")
        (C_UNINSTALL, "service is uninstall")
        (C_STOP, "service is stop")
        (C_RUN, "service is run");

    auto result = options.parse(argc, argv);
    if (result.count(C_HELP))
    {
        std::cout << options.help() << std::endl;

        return false;
    }
    else if (result.count(C_INSTALL))
    {
        bool bSuccess = Installer::Install(GetServiceName().c_str(), GetDisplayName().c_str(), GetStartType(), GetDependencies().c_str(), GetAccount().c_str(), GetPassword().c_str());
        if (bSuccess)
        {
            logger.LogInfo("ServiceMode", "Service is installed. ServiceName : {}, ServiceVersion : {}", GetServiceNameAnsi(), FileVersion());
            WriteEventLogEntry(std::format(L"{} is installed.", GetServiceName()), EVENTLOG_INFORMATION_TYPE);
        }
        else
        {
            logger.LogError("ServiceMode", "Service is not installed. ServiceName : {}, ServiceVersion : {}", GetServiceNameAnsi(), FileVersion());
            WriteEventLogEntry(L"Columbus service is install failed.", EVENTLOG_ERROR_TYPE);
        }

        std::cout << "service is install. : " << bSuccess << std::endl;

    }
    else if (result.count(C_UNINSTALL))
    {
        bool bSuccess = Installer::UnInstall(GetServiceName().c_str());
        if (bSuccess)
        {
            logger.LogInfo("ServiceMode", "Service is uninstalled. ServiceName : {}, ServiceVersion : {}", GetServiceNameAnsi(), FileVersion());

            std::cout << GetServiceNameAnsi() << " is uninstalled." << std::endl;

            WriteEventLogEntry(std::format(L"Service is uninstall.", GetServiceName()), EVENTLOG_INFORMATION_TYPE);
        }
        else
        {
            logger.LogError("ServiceMode", "Service is not uninstalled. ServiceName : {}, ServiceVersion : {}", GetServiceNameAnsi(), FileVersion());

            std::cout << GetServiceNameAnsi() << " is not uninstalled." << std::endl;

            WriteEventLogEntry(std::format(L"Service is not uninstall.", GetServiceName()), EVENTLOG_ERROR_TYPE);
        }
    }
    else if (result.count(C_STOP))
    {
        bool bSuccess = Installer::Stop(GetServiceName().c_str());
        if (bSuccess)
        {
            logger.LogInfo("ServiceMode", "{} is stopped.", GetServiceNameAnsi());

            std::cout << "service is stop." << std::endl;

            WriteEventLogEntry(std::format(L"Service is stop. ServiceName : {}", GetServiceName()), EVENTLOG_INFORMATION_TYPE);
        }
        else
        {
            logger.LogError("ServiceMode", "{} is not stopped.", GetServiceNameAnsi());

            std::cout << "service is not stop." << std::endl;
            WriteEventLogEntry(std::format(L"Service is stop. ServiceName : {}", GetServiceName()), EVENTLOG_ERROR_TYPE);
        }
    }

    bool bSuccess = ServiceMode::Run(std::dynamic_pointer_cast<ServiceMode>(shared_from_this()));
    if (bSuccess)
    {
        logger.LogInfo("ServiceMode", "Service running success. Service Name : {}, Version : {}", GetServiceNameAnsi(), FileVersion());
    }
    else
    {
        logger.LogError("ServiceMode", "Service run is failed. Service Name : {}, Version : {}", GetServiceNameAnsi(), FileVersion());
    }


    return true;
}


bool ServiceMode::Run(std::shared_ptr<ServiceMode> pService)
{
    auto& logger = LibCommons::Logger::GetInstance();
    m_pServiceMode = pService;

    logger.LogInfo("ServiceMode", "ServiceName : {}, Version : {}, Request Run.", m_pServiceMode->GetServiceNameAnsi(), m_pServiceMode->FileVersion());

    bool bSuccess = false;
#if _DEBUG
    m_pServiceMode->OnStarted();
    bSuccess = true;
#else 
    SERVICE_TABLE_ENTRY entries[] = {
        { const_cast<wchar_t*>(m_pServiceMode->GetServiceName().c_str()), (LPSERVICE_MAIN_FUNCTION)ServiceMain },
        { nullptr, nullptr }
    };

    bSuccess = StartServiceCtrlDispatcher(entries);
    if (!bSuccess)
    {
        m_pServiceMode->WriteEventLogEntry(StrConverter::ToUnicode(std::format("Service Mode is Started failed, Version : {}, Error : {}", m_pServiceMode->FileVersion(), ::GetLastError())), EVENTLOG_INFORMATION_TYPE);

        logger.LogError("ServiceMode", "StartServiceCtrlDispatcher is error. ServiceName : {}, Error : {}", m_pServiceMode->GetServiceNameAnsi(), ::GetLastError());
    }
    else
    {
        logger.LogInfo("ServiceMode", "StartServiceCtrlDispatcher is success. ServiceName : {}", m_pServiceMode->GetServiceNameAnsi());

        m_pServiceMode->WriteEventLogEntry(StrConverter::ToUnicode(std::format("Service Mode is Started, Version : {}", m_pServiceMode->FileVersion())), EVENTLOG_INFORMATION_TYPE);
    }

#endif

    return bSuccess;
}


void ServiceMode::Wait() const
{
    while (m_bRunning)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void WINAPI ServiceMode::ServiceMain(DWORD argc, TCHAR* argv[])
{
    LibCommons::Logger::GetInstance().LogInfo("ServiceMode", "Service Name : {}, ServiceMain function.", m_pServiceMode->GetServiceNameAnsi());

    m_pServiceMode->m_StatusHandle = ::RegisterServiceCtrlHandler(m_pServiceMode->GetServiceName().c_str(), ServiceCtrlHandler);
    if (nullptr == m_pServiceMode->m_StatusHandle)
    {

        m_pServiceMode->WriteEventLogEntry(StrConverter::ToUnicode(std::format("Service Name : {}, RegisterServiceCtrlHandler is failed. Error : {}", m_pServiceMode->GetServiceNameAnsi(), ::GetLastError())), EVENTLOG_ERROR_TYPE);

        throw ::GetLastError();
    }

    // Start the service.
    m_pServiceMode->Start(argc, argv);
}




//----------------------------------------------------------------
//
//   FUNCTION: CServiceBase::SetServiceStatus(DWORD, DWORD, DWORD)
//
//   PURPOSE: The function sets the service status and reports the status to 
//   the SCM.
//
//   PARAMETERS:
//   * dwCurrentState - the state of the service
//   * dwWin32ExitCode - error code to report
//   * dwWaitHint - estimated time for pending operation, in milliseconds
//
void ServiceMode::SetServiceStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode /*= NO_ERROR*/, DWORD dwWaitHint /*= 0*/)
{
    static DWORD dwCheckPoint = 1;

    // Fill in the SERVICE_STATUS structure of the service.
    m_Status.dwCurrentState = dwCurrentState;
    m_Status.dwWin32ExitCode = dwWin32ExitCode;
    m_Status.dwWaitHint = dwWaitHint;

    m_Status.dwCheckPoint = ((dwCurrentState == SERVICE_RUNNING) || (dwCurrentState == SERVICE_STOPPED)) ? 0 : dwCheckPoint++;

    // Report the status of the service to the SCM
    ::SetServiceStatus(m_StatusHandle, &m_Status);
}


bool ServiceMode::GetServiceBinaryName(std::string& rfOutServiceBinaryName)
{
    SC_HANDLE hSCManager = ::OpenSCManager(nullptr, nullptr, GENERIC_READ);
    if (nullptr == hSCManager)
    {
        WriteEventLogEntry(StrConverter::ToUnicode(std::format("OpenSCManager is not success, Name : {}, Version : {}, Error : {}", GetServiceNameAnsi(), FileVersion(), ::GetLastError())), EVENTLOG_ERROR_TYPE);

        return false;
    }

    SC_HANDLE hService = ::OpenService(hSCManager, GetServiceName().c_str(), SERVICE_QUERY_CONFIG);
    if (nullptr == hService)
    {
        WriteEventLogEntry(StrConverter::ToUnicode(std::format("OpenService is not success, Name : {}, Version : {}, Error : {}", GetServiceNameAnsi(), FileVersion(), ::GetLastError())), EVENTLOG_ERROR_TYPE);

        ::CloseServiceHandle(hSCManager);

        return false;
    }

    std::vector<BYTE> buffers;
    DWORD dwBytesNeeded = sizeof(QUERY_SERVICE_CONFIG);
    LPQUERY_SERVICE_CONFIG pConfig = {};
    do
    {
        buffers.resize(dwBytesNeeded);
        pConfig = reinterpret_cast<LPQUERY_SERVICE_CONFIG>(&buffers[0]);
        if (::QueryServiceConfig(hService, pConfig, static_cast<DWORD>(buffers.size()), &dwBytesNeeded))
        {
            rfOutServiceBinaryName = StrConverter::ToAnsi(std::wstring(pConfig->lpBinaryPathName));
            break;
        }

    } while (GetLastError() == ERROR_INSUFFICIENT_BUFFER);


    return true;
}

bool ServiceMode::GetServiceBinaryLocation(std::string& rfOutServiceBinaryLocation)
{
    std::string binaryFullPath;
    if (!GetServiceBinaryName(binaryFullPath))
    {
        WriteEventLogEntry(StrConverter::ToUnicode(std::format("GetServiceBinaryName is not success, Name : {}, Version : {}, Error : {}", GetServiceNameAnsi(), FileVersion(), ::GetLastError())), EVENTLOG_ERROR_TYPE);

        return false;
    }

    std::filesystem::path p(binaryFullPath);
    if (!p.has_filename())
    {
        return false;
    }

    rfOutServiceBinaryLocation = p.remove_filename().string();

    return true;
}

void ServiceMode::WriteEventLogEntry(std::wstring message, WORD wType)
{
    WriteEventLogEntry(const_cast<TCHAR*>(message.c_str()), wType);
}


//
//   FUNCTION: CServiceBase::WriteEventLogEntry(PWSTR, WORD)w
//
//   PURPOSE: Log a message to the Application event log.
//
//   PARAMETERS:
//   * pszMessage - string message to be logged.
//   * wType - the type of event to be logged. The parameter can be one of 
//     the following values.
//
//     EVENTLOG_SUCCESS
//     EVENTLOG_AUDIT_FAILURE
//     EVENTLOG_AUDIT_SUCCESS
//     EVENTLOG_ERROR_TYPE
//     EVENTLOG_INFORMATION_TYPE
//     EVENTLOG_WARNING_TYPE
//
void ServiceMode::WriteEventLogEntry(PWSTR pszMessage, WORD wType)
{
    HANDLE hEventSource = nullptr;
    LPCWSTR lpszStrings[2] = { nullptr, nullptr };

    const auto serviceName = GetServiceName();

    hEventSource = ::RegisterEventSource(nullptr, serviceName.c_str());
    if (hEventSource)
    {
        lpszStrings[0] = serviceName.c_str();
        lpszStrings[1] = pszMessage;

        ::ReportEvent(hEventSource,  // Event log handle
            wType,                 // Event type
            0,                     // Event category
            0,                     // Event identifier
            nullptr,                  // No security identifier
            2,                     // Size of lpszStrings array
            0,                     // No binary data
            lpszStrings,           // Array of strings
            nullptr                   // No binary data
        );

        ::DeregisterEventSource(hEventSource);
    }
}

//
//   FUNCTION: CServiceBase::WriteErrorLogEntry(PWSTR, DWORD)
//
//   PURPOSE: Log an error message to the Application event log.
//
//   PARAMETERS:
//   * pszFunction - the function that gives the error
//   * dwError - the error code
//
void ServiceMode::WriteErrorLogEntry(PWSTR pszFunction, DWORD dwError /*= ::GetLastError()*/)
{
    wchar_t szMessage[260] = {};
    ::swprintf_s(szMessage, 260, L"%s failed w/err 0x%08lx", pszFunction, dwError);

    WriteEventLogEntry(szMessage, EVENTLOG_ERROR_TYPE);
}

//----------------------------------------------------------------
void ServiceMode::Start(DWORD argc, PWSTR* argv)
{
    try
    {
        // Tell SCM that the service is starting.
        SetServiceStatus(SERVICE_START_PENDING);

        std::wstring message = GetServiceName() + L" is Started";
        WriteEventLogEntry(const_cast<TCHAR*>(message.c_str()), EVENTLOG_INFORMATION_TYPE);

        // Perform service-specific initialization.
        OnStarted();

        // Tell SCM that the service is started.
        SetServiceStatus(SERVICE_RUNNING);
    }
    catch (DWORD dwError)
    {
        // Log the error.
        std::wstring message = GetServiceName() + L" Start";
        WriteErrorLogEntry(const_cast<TCHAR*>(message.c_str()), dwError);

        // Set the service status to be stopped.
        SetServiceStatus(SERVICE_STOPPED, dwError);
    }
    catch (...)
    {
        // Log the error.
        WriteEventLogEntry(L"Service failed to start.", EVENTLOG_ERROR_TYPE);

        // Set the service status to be stopped.
        SetServiceStatus(SERVICE_STOPPED);
    }
}

void ServiceMode::Pause()
{
    std::wstring message = GetServiceName() + L" is Paused";

    try
    {
        // Tell SCM that the service is pausing.
        SetServiceStatus(SERVICE_PAUSE_PENDING);

        WriteEventLogEntry(const_cast<TCHAR*>(message.c_str()), EVENTLOG_INFORMATION_TYPE);

        // Perform service-specific pause operations.
        OnPaused();

        // Tell SCM that the service is paused.
        SetServiceStatus(SERVICE_PAUSED);
    }
    catch (DWORD dwError)
    {
        // Log the error.
        WriteErrorLogEntry(const_cast<TCHAR*>(message.c_str()), dwError);

        // Tell SCM that the service is still running.
        SetServiceStatus(SERVICE_RUNNING);
    }
    catch (...)
    {
        // Log the error.
        WriteEventLogEntry(L"Service failed to pause.", EVENTLOG_ERROR_TYPE);

        // Tell SCM that the service is still running.
        SetServiceStatus(SERVICE_RUNNING);
    }
}

void ServiceMode::Stop()
{
    std::wstring message = GetServiceName() + L" is Stopped";

    DWORD dwOriginalState = m_Status.dwCurrentState;
    try
    {
        // Tell SCM that the service is stopping.
        SetServiceStatus(SERVICE_STOP_PENDING);


        WriteEventLogEntry(const_cast<TCHAR*>(message.c_str()), EVENTLOG_INFORMATION_TYPE);

        // Perform service-specific stop operations.
        OnStopped();

        // Tell SCM that the service is stopped.
        SetServiceStatus(SERVICE_STOPPED);
    }
    catch (DWORD dwError)
    {
        // Log the error.
        WriteErrorLogEntry(const_cast<TCHAR*>(message.c_str()), dwError);

        // Set the orginal service status.
        SetServiceStatus(dwOriginalState);
    }
    catch (...)
    {
        // Log the error.
        WriteEventLogEntry(L"Service failed to stop.", EVENTLOG_ERROR_TYPE);

        // Set the orginal service status.
        SetServiceStatus(dwOriginalState);
    }
}

void ServiceMode::Continue()
{
    std::wstring message = GetServiceName() + L" is continue";
    try
    {
        SetServiceStatus(SERVICE_CONTINUE_PENDING);

        WriteEventLogEntry(const_cast<TCHAR*>(message.c_str()), EVENTLOG_INFORMATION_TYPE);

        OnContinue();
        SetServiceStatus(SERVICE_RUNNING);
    }
    catch (DWORD dwError)
    {
        // Log the error
        WriteErrorLogEntry(const_cast<TCHAR*>(message.c_str()), dwError);

        // Tell SCM that the service still paused.
        SetServiceStatus(SERVICE_PAUSED);
    }
    catch (...)
    {
        // Log the error.
        WriteEventLogEntry(L"Service failed to resume.", EVENTLOG_ERROR_TYPE);

        // Tell SCM that the service is still paused.
        SetServiceStatus(SERVICE_PAUSED);
    }
}

//
//   FUNCTION: CServiceBase::Shutdown()
//
//   PURPOSE: The function executes when the system is shutting down. It 
//   calls the OnShutdown virtual function in which you can specify what 
//   should occur immediately prior to the system shutting down. If an error 
//   occurs, the error will be logged in the Application event log.
//
void ServiceMode::Shutdown()
{
    std::wstring message = GetServiceName() + L" is shutdown";

    try
    {

        WriteEventLogEntry(const_cast<TCHAR*>(message.c_str()), EVENTLOG_INFORMATION_TYPE);

        OnShutdown();

        SetServiceStatus(SERVICE_STOPPED);
    }
    catch (DWORD dwError)
    {
        WriteErrorLogEntry(const_cast<TCHAR*>(message.c_str()), dwError);
    }
    catch (...)
    {
        WriteEventLogEntry(L"Service failed to shut down.", EVENTLOG_ERROR_TYPE);
    }
}


void WINAPI ServiceMode::ServiceCtrlHandler(DWORD dwCtrl)
{
    switch (dwCtrl)
    {
    case SERVICE_CONTROL_STOP:
        m_pServiceMode->Stop();
        break;

    case SERVICE_CONTROL_PAUSE:
        m_pServiceMode->Pause();
        break;

    case SERVICE_CONTROL_CONTINUE:
        m_pServiceMode->Continue();
        break;

    case SERVICE_CONTROL_SHUTDOWN:
        m_pServiceMode->Shutdown();
        break;

    case SERVICE_CONTROL_INTERROGATE:
        break;
    default: break;
    }
}


} // namespace LibCommons} // namespace LibCommons