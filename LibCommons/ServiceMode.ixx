module;

#include <Windows.h>
#include <memory>
#include <string>

export module commons.service_mode;


import commons.strconverter; 

namespace LibCommons
{



export class ServiceMode : public std::enable_shared_from_this<ServiceMode>
{
public:
    class Installer
    {
    public:

        static bool Install(LPCWSTR ServiceName, LPCWSTR DisplayName, DWORD dwStartType, LPCWSTR Dependencies, LPCWSTR Account, LPCWSTR password);

        static bool Stop(LPCWSTR ServiceName);

        static bool UnInstall(LPCWSTR ServiceName);
    };

    ServiceMode(const bool bCanStop = true, const bool bCanShutdown = true, const bool bCanPauseContainue = false);

    bool Execute(const DWORD argc, const char* argv[]);


    static bool Run(std::shared_ptr<ServiceMode> pService);

    void Wait() const;

    std::string FileVersion() const { return m_FileVersion; }
    void FileVersion(std::string val) { m_FileVersion = val; }

    // Get Service Binary Location
    bool GetServiceBinaryName(std::string& rfOutServiceBinaryName);

    bool GetServiceBinaryLocation(std::string& rfOutServiceBinaryLocation);

    void WriteEventLogEntry(PWSTR pszMessage, WORD wType);
    void WriteEventLogEntry(std::wstring message, WORD wType);
    void WriteErrorLogEntry(PWSTR pszFunction, DWORD dwError = ::GetLastError());

    bool IsRunnig() const { return m_bRunning; }

protected:
    //----------------------------------------------------------------
    virtual void OnStarted() = 0;

    virtual void OnStopped() = 0;

    virtual void OnPaused() {}

    virtual void OnContinue() {}

    virtual void OnShutdown() = 0;

    virtual std::wstring GetServiceName() const = 0;
    virtual std::wstring GetDisplayName() = 0;
    virtual const DWORD GetStartType() const = 0;


    std::string GetServiceNameAnsi() const { return StrConverter::ToAnsi(GetServiceName()); }

    std::string GetDisplayNameAnsi() { return StrConverter::ToAnsi(GetDisplayName()); }
    //----------------------------------------------------------------

    virtual std::wstring GetDependencies() { return L""; }
    virtual std::wstring GetAccount() const { return L"NT AUTHORITY\\localService"; }
    virtual std::wstring GetPassword() const { return L""; };

    void SetServiceStatus(DWORD dwCurrentState, DWORD dwWin32ExitCode = NO_ERROR, DWORD dwWaitHint = 0);

private:

    void Start(DWORD argc, PWSTR* argv);

    void Pause();
    void Stop();
    void Continue();
    void Shutdown();

    static void WINAPI ServiceMain(DWORD argc, TCHAR* argv[]);

    static void WINAPI ServiceCtrlHandler(DWORD dwCtrl);

protected:
    bool m_bRunning = false;
private:

    static std::shared_ptr<ServiceMode> m_pServiceMode;

    std::string m_FileVersion;
    std::string m_ServiceName;

    // The status of the service.
    SERVICE_STATUS m_Status = {};

    // The service status handle.
    SERVICE_STATUS_HANDLE m_StatusHandle = {};

    const std::string C_INSTALL = "install";
    const std::string C_UNINSTALL = "uninstall";
    const std::string C_STOP = "stop";
    const std::string C_HELP = "help";
    const std::string C_RUN = "run";

};

} // namespace LibCommons