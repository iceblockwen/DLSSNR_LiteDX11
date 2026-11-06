#pragma once
#include <windows.h>

namespace lite
{
enum class State : unsigned
{
    Waiting,
    Ready,
    Disabled,
    Failed
};
// One switch for every log line in the plugin.
// Off: no log file is written and no message is formatted.
// Basic: startup, identity, state changes and failures.
// Verbose: reserved for per-frame traces; nothing else is gated by it.
enum class Detail : unsigned
{
    Off = 0,
    Basic = 1,
    Verbose = 2
};
// Fixed-size, versioned snapshot. No pointers into runtime-owned memory.
struct Status
{
    unsigned size = sizeof(Status);
    unsigned version = 1;
    State loader = State::Waiting;
    DWORD error = ERROR_SUCCESS;
    bool enabled = true;
    bool logging = true;
    wchar_t pluginPath[1024]{};
    wchar_t systemPath[1024]{};
};
void Initialize(HMODULE plugin, HMODULE system);
// L2 entry points implemented by hooks.cpp.
void InstallCapture();
void InstallD3D11Capture(HMODULE module);
void Fail(DWORD error);
Status Snapshot();
// Lock-free hot-path queries; Snapshot copies the full status block.
bool Enabled();
bool Usable();
Detail DetailLevel() noexcept;
// The log queue and its worker are kept although no call site exists: adding a
// diagnostic means calling Log without rebuilding the plumbing.
void Log(const wchar_t* message) noexcept;
void LogVerbose(const wchar_t* message) noexcept;
}
