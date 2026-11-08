// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// Running a program as a system service, and installing and controlling one.
//
// run() is the one call every platform has: it runs the program's body and delivers the request to stop (Ctrl-C and SIGTERM in
// a console; on Windows, also the service manager's Stop and Shutdown) as a call to the function the body registered, so that a
// program has one way to end whether a person or the system started it.
//
// install(), remove(), start(), stop() and state() drive the Windows service control manager. Other systems have their own
// (systemd units, launchd plists) and are not driven from here: there kSupported is false and these throw Unimplemented.
//
//     int main(int argc, char** argv) {
//         const bool asService = argc > 1 && std::string(argv[1]) == "--service";
//         return sys::service::run("my-service", asService, [](sys::service::Control& control) {
//             Server server;                       // may throw: the service then ends with an error
//             control.onStop([&] { server.stop(); });
//             control.running();                   // up: the service manager is told it started
//             server.run();                        // returns once stop() was called
//             return 0;
//         });
//     }
//
// Failures throw base::Error: PermissionDenied for what needs an administrator, NotFound for a service that is not installed,
// AlreadyExists for one that is, Internal for the rest, with the system's error code in detail().
#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace sys::service {

#ifdef _WIN32
inline constexpr bool kSupported = true;
#else
inline constexpr bool kSupported = false;
#endif

/// What run()'s body gets.
class Control {
public:
    virtual ~Control() = default;
    /// Registers what makes the body return: called on a thread of the library's, once, when the program is asked to stop
    /// (immediately if it already was). Register it before running().
    virtual void onStop(std::function<void()> stop) = 0;
    /// The program is up. Under a service manager this ends the "starting" state; elsewhere it does nothing. A body that
    /// returns without calling it has failed to start.
    virtual void running() = 0;
    /// Whether a stop was asked for.
    virtual bool stopRequested() const = 0;
};

/// Runs `body` and returns its result, which becomes the exit code. With `asService` the process was started by the Windows
/// service control manager, to which this connects as service `name` (throws Unavailable when it was not). Otherwise, or on
/// another system, it runs in the foreground and stops on Ctrl-C and SIGTERM. An exception from `body` is reported (to the
/// event log under a service, to stderr otherwise) and gives exit code 1.
int run(const std::string& name, bool asService, const std::function<int(Control&)>& body);

/// A command-line word as CommandLineToArgvW (and so every program's startup code) reads it back.
std::string quoteArgument(const std::string& argument);

struct Spec {
    std::string name;         ///< the service's key: letters, digits, '-' and '_'
    std::string displayName;  ///< shown by services.msc; the name when empty
    std::string description;
    /// The program; empty is this one. The command line is it, then `arguments`, each quoted.
    std::string executable;
    std::vector<std::string> arguments;
    /// "" is the virtual account "NT SERVICE\<name>", which has no password and no rights but the ones given below;
    /// "LocalSystem" (all rights on the machine), "LocalService" or "NetworkService"; or "DOMAIN\user" or ".\user" with
    /// `password`.
    std::string account;
    std::string password;
    bool autoStart = true;     ///< at boot (delayed, so the network is up); else only when started
    bool restartOnFailure = true;
    /// "NAME=value" for the service's environment.
    std::vector<std::string> environment;

    struct Access {
        std::string path;
        bool write = false;  ///< read and write (and delete) instead of read; a directory's applies to what is in it
    };
    /// Rights added to the account on existing files and directories. Nothing is taken away from anyone.
    std::vector<Access> access;
    /// Directories to make, if they are missing, readable and writable by the account, Administrators and SYSTEM only.
    /// One that exists is left as it is (grant it with `access`).
    std::vector<std::string> privateDirectories;
};

/// Installs the service (stopped). If anything after creating it fails, it is removed again.
void install(const Spec& spec);
/// Stops the service if it runs, and removes it.
void remove(const std::string& name);
/// Starts the service and waits until it runs or `timeout` passes.
void start(const std::string& name, std::chrono::milliseconds timeout = std::chrono::seconds(30));
/// Asks the service to stop and waits until it has or `timeout` passes.
void stop(const std::string& name, std::chrono::milliseconds timeout = std::chrono::seconds(30));

enum class State { NotInstalled, Stopped, Starting, Stopping, Running, Paused, Unknown };
State state(const std::string& name);
const char* stateName(State state) noexcept;

/// Whether this process runs with an administrator's full rights (what install and remove need).
bool isElevated() noexcept;

/// The full path of the running program ("" if the system will not say).
std::string executablePath();

}  // namespace sys::service
