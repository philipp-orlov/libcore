// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// sys/service.hpp on Windows: the service host (the control handler and ServiceMain that the service control manager
// calls), and installing and controlling services through it.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <winsvc.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/status.hpp"
#include "service_internal.hpp"
#include "sys/service.hpp"

#pragma comment(lib, "advapi32.lib")

namespace sys::service {

namespace {

using base::Code;
using base::Error;

std::wstring widen(const std::string& text) {
    if (text.empty())
        return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return wide;
}

std::string narrow(const std::wstring& text) {
    if (text.empty())
        return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::string systemMessage(DWORD error) {
    char* text = nullptr;
    const DWORD n = ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, error, 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string message;
    if (n > 0 && text) {
        message.assign(text, n);
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' ' || message.back() == '.'))
            message.pop_back();
    } else {
        message = "error " + std::to_string(error);
    }
    if (text)
        ::LocalFree(text);
    return message;
}

// base::Status keeps 87 bytes of message: the action is short, and the system's words are cut first.
[[noreturn]] void fail(const std::string& action, DWORD error = ::GetLastError()) {
    Code code = Code::Internal;
    std::string text = systemMessage(error);
    switch (error) {
        case ERROR_ACCESS_DENIED:
            code = Code::PermissionDenied;
            text = "access denied; run it as an administrator";
            break;
        case ERROR_SERVICE_DOES_NOT_EXIST: code = Code::NotFound; text = "no such service"; break;
        case ERROR_SERVICE_EXISTS: code = Code::AlreadyExists; text = "the service is already installed"; break;
        case ERROR_SERVICE_MARKED_FOR_DELETE: code = Code::FailedPrecondition; text = "the service is being deleted; close services.msc"; break;
        case ERROR_SERVICE_LOGON_FAILED:
        case ERROR_INVALID_SERVICE_ACCOUNT: code = Code::InvalidArgument; text = "the account or password is wrong"; break;
        default: break;
    }
    throw Error(code, action + ": " + text, error);
}

class ScHandle {
public:
    ScHandle() = default;
    explicit ScHandle(SC_HANDLE handle) : handle_(handle) {}
    ~ScHandle() {
        if (handle_)
            ::CloseServiceHandle(handle_);
    }
    ScHandle(ScHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    ScHandle& operator=(ScHandle&& other) noexcept {
        if (this != &other) {
            if (handle_)
                ::CloseServiceHandle(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    SC_HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    SC_HANDLE handle_ = nullptr;
};

ScHandle openManager(DWORD access) {
    ScHandle manager(::OpenSCManagerW(nullptr, nullptr, access));
    if (!manager)
        fail("cannot reach the service manager");
    return manager;
}

ScHandle openService(const ScHandle& manager, const std::string& name, DWORD access) {
    ScHandle service(::OpenServiceW(manager.get(), widen(name).c_str(), access));
    if (!service)
        fail("service " + name);
    return service;
}

// ---- the host -----------------------------------------------------------------------------------------------------

struct Host {
    std::wstring name;
    std::function<int(Control&)> body;
    SERVICE_STATUS_HANDLE handle = nullptr;
    detail::BasicControl control;
    std::mutex mutex;
    DWORD state = SERVICE_START_PENDING;
    DWORD checkpoint = 0;
};

Host* gHost = nullptr;

void report(DWORD state, DWORD win32Exit = NO_ERROR, DWORD specificExit = 0, DWORD waitHint = 0) {
    Host& host = *gHost;
    std::lock_guard<std::mutex> lock(host.mutex);
    host.state = state;
    SERVICE_STATUS status{};
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    status.dwWin32ExitCode = win32Exit;
    status.dwServiceSpecificExitCode = specificExit;
    const bool pending = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING;
    status.dwCheckPoint = pending ? ++host.checkpoint : 0;
    status.dwWaitHint = pending ? waitHint : 0;
    ::SetServiceStatus(host.handle, &status);
}

// Something for the person who looks at the event log: a service has no console.
void logError(const std::wstring& source, const std::string& text) {
    HANDLE log = ::RegisterEventSourceW(nullptr, source.c_str());
    if (!log)
        return;
    const std::wstring wide = widen(text);
    const wchar_t* strings[1] = {wide.c_str()};
    ::ReportEventW(log, EVENTLOG_ERROR_TYPE, 0, 1, nullptr, 1, 0, strings, nullptr);
    ::DeregisterEventSource(log);
}

DWORD WINAPI onControl(DWORD control, DWORD, LPVOID, LPVOID) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            report(SERVICE_STOP_PENDING, NO_ERROR, 0, 30000);
            gHost->control.requestStop();
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            report(gHost->state);
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI serviceMain(DWORD, LPWSTR*) {
    Host& host = *gHost;
    host.handle = ::RegisterServiceCtrlHandlerExW(host.name.c_str(), onControl, nullptr);
    if (!host.handle)
        return;
    report(SERVICE_START_PENDING, NO_ERROR, 0, 15000);
    host.control.runningHook = [] { report(SERVICE_RUNNING); };
    int code = 1;
    try {
        code = host.body(host.control);
    } catch (const std::exception& error) {
        logError(host.name, error.what());
        code = 1;
    }
    report(SERVICE_STOPPED, code == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR, static_cast<DWORD>(code));
}

// ---- security -----------------------------------------------------------------------------------------------------

std::wstring defaultAccount(const Spec& spec) { return L"NT SERVICE\\" + widen(spec.name); }

// The name to put in an access control entry, or "" for LocalSystem, which can reach everything anyway.
std::wstring trusteeOf(const Spec& spec) {
    if (spec.account.empty())
        return defaultAccount(spec);
    if (spec.account == "LocalSystem")
        return {};
    if (spec.account == "LocalService")
        return L"NT AUTHORITY\\LocalService";
    if (spec.account == "NetworkService")
        return L"NT AUTHORITY\\NetworkService";
    return widen(spec.account);
}

void grant(const std::string& path, const std::wstring& trustee, bool write) {
    const std::wstring wpath = widen(std::filesystem::absolute(std::filesystem::u8path(path)).u8string());
    const DWORD attributes = ::GetFileAttributesW(wpath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        fail("cannot find " + path, ::GetLastError() == ERROR_FILE_NOT_FOUND ? ERROR_FILE_NOT_FOUND : ::GetLastError());
    const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    PACL oldAcl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    DWORD result = ::GetNamedSecurityInfoW(wpath.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldAcl,
                                           nullptr, &descriptor);
    if (result != ERROR_SUCCESS)
        fail("cannot read the rights of " + path, result);
    std::unique_ptr<void, decltype(&::LocalFree)> descriptorGuard(descriptor, ::LocalFree);

    EXPLICIT_ACCESSW entry{};
    entry.grfAccessPermissions = write ? (FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE)
                                       : (FILE_GENERIC_READ | FILE_GENERIC_EXECUTE);
    entry.grfAccessMode = GRANT_ACCESS;
    entry.grfInheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    ::BuildTrusteeWithNameW(&entry.Trustee, const_cast<LPWSTR>(trustee.c_str()));

    PACL newAcl = nullptr;
    result = ::SetEntriesInAclW(1, &entry, oldAcl, &newAcl);
    if (result != ERROR_SUCCESS)
        fail("cannot add the account to " + path, result);
    std::unique_ptr<void, decltype(&::LocalFree)> aclGuard(newAcl, ::LocalFree);
    result = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(wpath.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                     newAcl, nullptr);
    if (result != ERROR_SUCCESS)
        fail("cannot set the rights of " + path, result);
}

std::wstring sidString(const std::wstring& account) {
    DWORD sidSize = 0, domainSize = 0;
    SID_NAME_USE use;
    ::LookupAccountNameW(nullptr, account.c_str(), nullptr, &sidSize, nullptr, &domainSize, &use);
    if (sidSize == 0)
        fail("unknown account " + narrow(account));
    std::vector<char> sid(sidSize);
    std::vector<wchar_t> domain(domainSize);
    if (!::LookupAccountNameW(nullptr, account.c_str(), sid.data(), &sidSize, domain.data(), &domainSize, &use))
        fail("unknown account " + narrow(account));
    LPWSTR text = nullptr;
    if (!::ConvertSidToStringSidW(sid.data(), &text))
        fail("cannot read the account's id");
    std::wstring out(text);
    ::LocalFree(text);
    return out;
}

void makePrivateDirectory(const std::string& path, const std::wstring& trustee) {
    const std::filesystem::path directory = std::filesystem::absolute(std::filesystem::u8path(path));
    std::error_code ignored;
    if (std::filesystem::exists(directory, ignored))
        return;  // somebody's directory: not ours to change (`access` adds the account to it)
    std::filesystem::create_directories(directory, ignored);
    if (!std::filesystem::is_directory(directory, ignored))
        fail("cannot create " + path, ERROR_PATH_NOT_FOUND);
    // Protected (it takes nothing from its parent), full control for SYSTEM and Administrators and the service's account.
    std::wstring sddl = L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
    if (!trustee.empty())
        sddl += L"(A;OICI;FA;;;" + sidString(trustee) + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        fail("cannot build the rights of " + path);
    std::unique_ptr<void, decltype(&::LocalFree)> guard(descriptor, ::LocalFree);
    BOOL present = FALSE, defaulted = FALSE;
    PACL acl = nullptr;
    ::GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted);
    const std::wstring wpath = directory.wstring();
    const DWORD result = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(wpath.c_str()), SE_FILE_OBJECT,
                                                 DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                                 acl, nullptr);
    if (result != ERROR_SUCCESS)
        fail("cannot set the rights of " + path, result);
}

void setEnvironment(const std::string& name, const std::vector<std::string>& variables) {
    if (variables.empty())
        return;
    std::wstring block;
    for (const std::string& variable : variables) {
        block += widen(variable);
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    HKEY key = nullptr;
    const std::wstring path = L"SYSTEM\\CurrentControlSet\\Services\\" + widen(name);
    LONG result = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_SET_VALUE, &key);
    if (result != ERROR_SUCCESS)
        fail("cannot open the service's key", static_cast<DWORD>(result));
    result = ::RegSetValueExW(key, L"Environment", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(block.data()),
                              static_cast<DWORD>(block.size() * sizeof(wchar_t)));
    ::RegCloseKey(key);
    if (result != ERROR_SUCCESS)
        fail("cannot set the service's environment", static_cast<DWORD>(result));
}

std::wstring currentExecutable() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (n == 0)
            fail("cannot find this program");
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

bool validName(const std::string& name) {
    if (name.empty() || name.size() > 200)
        return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    });
}

// Waits for the service to leave a pending state into `wanted`, or for the time to run out. Returns the last status.
SERVICE_STATUS_PROCESS waitFor(const ScHandle& service, DWORD wanted, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    SERVICE_STATUS_PROCESS status{};
    for (;;) {
        DWORD needed = 0;
        if (!::QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof status, &needed))
            fail("cannot read the service's state");
        if (status.dwCurrentState == wanted)
            return status;
        // A start that fell back to stopped will not get there.
        const bool failed = wanted == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED;
        if (failed || std::chrono::steady_clock::now() >= deadline)
            return status;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

}  // namespace

namespace detail {

int runUnderServiceManager(const std::string& name, const std::function<int(Control&)>& body) {
    Host host;
    host.name = widen(name);
    host.body = body;
    gHost = &host;
    SERVICE_TABLE_ENTRYW table[] = {{host.name.data(), serviceMain}, {nullptr, nullptr}};
    // Returns when the service has stopped (and so after serviceMain did).
    if (!::StartServiceCtrlDispatcherW(table)) {
        const DWORD error = ::GetLastError();
        gHost = nullptr;
        if (error == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT)
            throw Error(Code::Unavailable, "not started by the service manager; run it in a console without --service", error);
        fail("cannot connect to the service manager", error);
    }
    gHost = nullptr;
    return 0;
}

}  // namespace detail

// ---- management ---------------------------------------------------------------------------------------------------

bool isElevated() noexcept {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = ::GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size) != 0;
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::string executablePath() { return narrow(currentExecutable()); }

void install(const Spec& spec) {
    if (!validName(spec.name))
        throw Error(Code::InvalidArgument, "a service name is letters, digits, '-', '_' and '.'");
    const std::wstring executable = spec.executable.empty() ? currentExecutable() : widen(spec.executable);
    std::string commandLine = quoteArgument(narrow(executable));
    for (const std::string& argument : spec.arguments)
        commandLine += " " + quoteArgument(argument);

    std::wstring startName;
    const wchar_t* account = nullptr;
    const wchar_t* password = nullptr;
    std::wstring widePassword = widen(spec.password);
    if (spec.account.empty()) {
        startName = defaultAccount(spec);
        account = startName.c_str();
    } else if (spec.account == "LocalSystem") {
        account = nullptr;  // the default of CreateService
    } else if (spec.account == "LocalService") {
        startName = L"NT AUTHORITY\\LocalService";
        account = startName.c_str();
    } else if (spec.account == "NetworkService") {
        startName = L"NT AUTHORITY\\NetworkService";
        account = startName.c_str();
    } else {
        startName = widen(spec.account);
        account = startName.c_str();
        password = widePassword.c_str();
    }

    ScHandle manager = openManager(SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    const std::wstring name = widen(spec.name);
    const std::wstring display = widen(spec.displayName.empty() ? spec.name : spec.displayName);
    const std::wstring command = widen(commandLine);
    ScHandle service(::CreateServiceW(manager.get(), name.c_str(), display.c_str(), SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                      spec.autoStart ? SERVICE_AUTO_START : SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
                                      command.c_str(), nullptr, nullptr, nullptr, account, password));
    if (!service)
        fail("cannot create service " + spec.name);

    // Whatever goes wrong from here, the half-made service goes too.
    bool done = false;
    struct Rollback {
        SC_HANDLE service;
        bool& done;
        ~Rollback() {
            if (!done)
                ::DeleteService(service);
        }
    } rollback{service.get(), done};

    if (!spec.description.empty()) {
        std::wstring text = widen(spec.description);
        SERVICE_DESCRIPTIONW description{text.data()};
        ::ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION, &description);
    }
    if (spec.autoStart) {
        SERVICE_DELAYED_AUTO_START_INFO delayed{TRUE};
        ::ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed);
    }
    if (spec.restartOnFailure) {
        SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 30000}, {SC_ACTION_RESTART, 60000}};
        SERVICE_FAILURE_ACTIONSW failure{};
        failure.dwResetPeriod = 24 * 60 * 60;
        failure.cActions = 3;
        failure.lpsaActions = actions;
        ::ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
        SERVICE_FAILURE_ACTIONS_FLAG flag{TRUE};  // also for an exit with an error code, not only for a crash
        ::ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag);
    }
    setEnvironment(spec.name, spec.environment);

    const std::wstring trustee = trusteeOf(spec);
    for (const std::string& directory : spec.privateDirectories)
        makePrivateDirectory(directory, trustee);
    if (!trustee.empty())
        for (const Spec::Access& access : spec.access)
            grant(access.path, trustee, access.write);
    done = true;
}

void remove(const std::string& name) {
    ScHandle manager = openManager(SC_MANAGER_CONNECT);
    ScHandle service = openService(manager, name, DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
    SERVICE_STATUS status{};
    if (::ControlService(service.get(), SERVICE_CONTROL_STOP, &status))
        waitFor(service, SERVICE_STOPPED, std::chrono::seconds(30));
    if (!::DeleteService(service.get()) && ::GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE)
        fail("cannot delete service " + name);
}

void start(const std::string& name, std::chrono::milliseconds timeout) {
    ScHandle manager = openManager(SC_MANAGER_CONNECT);
    ScHandle service = openService(manager, name, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!::StartServiceW(service.get(), 0, nullptr) && ::GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
        fail("cannot start service " + name);
    const SERVICE_STATUS_PROCESS status = waitFor(service, SERVICE_RUNNING, timeout);
    if (status.dwCurrentState == SERVICE_RUNNING)
        return;
    if (status.dwCurrentState == SERVICE_STOPPED)
        throw Error(Code::Internal,
                    "the service stopped while starting (exit code " +
                        std::to_string(status.dwWin32ExitCode == ERROR_SERVICE_SPECIFIC_ERROR ? status.dwServiceSpecificExitCode
                                                                                              : status.dwWin32ExitCode) +
                        "); see the event log",
                    status.dwWin32ExitCode);
    throw Error(Code::DeadlineExceeded, "the service did not start in time");
}

void stop(const std::string& name, std::chrono::milliseconds timeout) {
    ScHandle manager = openManager(SC_MANAGER_CONNECT);
    ScHandle service = openService(manager, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    SERVICE_STATUS ignored{};
    if (!::ControlService(service.get(), SERVICE_CONTROL_STOP, &ignored) && ::GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
        fail("cannot stop service " + name);
    if (waitFor(service, SERVICE_STOPPED, timeout).dwCurrentState != SERVICE_STOPPED)
        throw Error(Code::DeadlineExceeded, "the service did not stop in time");
}

State state(const std::string& name) {
    ScHandle manager = openManager(SC_MANAGER_CONNECT);
    ScHandle service(::OpenServiceW(manager.get(), widen(name).c_str(), SERVICE_QUERY_STATUS));
    if (!service) {
        if (::GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
            return State::NotInstalled;
        fail("service " + name);
    }
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    if (!::QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof status, &needed))
        fail("cannot read the service's state");
    switch (status.dwCurrentState) {
        case SERVICE_STOPPED: return State::Stopped;
        case SERVICE_START_PENDING: return State::Starting;
        case SERVICE_STOP_PENDING: return State::Stopping;
        case SERVICE_RUNNING: return State::Running;
        case SERVICE_PAUSED:
        case SERVICE_PAUSE_PENDING:
        case SERVICE_CONTINUE_PENDING: return State::Paused;
        default: return State::Unknown;
    }
}

}  // namespace sys::service
