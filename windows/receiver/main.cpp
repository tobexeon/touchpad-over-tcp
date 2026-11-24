#include "Mtp1.h"
#include "TouchSession.h"
#include "TouchpadIoctl.h"

#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

/* Loopback-only shutdown channel. Killing the process while a finger or a
   button is down would leave the driver holding the last report, so shutdown
   has to go through the same path as a disconnect: release, then exit. A
   connection to this port means "stop". */
constexpr std::uint16_t kControlPort = 39872;

class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
    }
    ~WinsockRuntime() { WSACleanup(); }
};

class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (value_ != INVALID_HANDLE_VALUE) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    [[nodiscard]] bool valid() const { return value_ != INVALID_HANDLE_VALUE; }
    [[nodiscard]] HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

std::string Narrow(std::wstring_view text) {
    std::string out;
    out.reserve(text.size());
    for (const wchar_t character : text) {
        out.push_back(character >= 0 && character < 128 ? static_cast<char>(character) : '?');
    }
    return out;
}

std::wstring Lowercase(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(character)));
    });
    return text;
}

std::wstring Widen(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size);
    return out;
}

std::vector<DWORD> FindProcessIds(const std::vector<std::wstring>& lowercase_names) {
    std::vector<DWORD> found;
    if (lowercase_names.empty()) return found;
    const Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid()) return found;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot.get(), &entry) != FALSE) {
        do {
            std::wstring candidate = Lowercase(entry.szExeFile);
            if (candidate.size() > 4 &&
                candidate.compare(candidate.size() - 4, 4, L".exe") == 0) {
                candidate.resize(candidate.size() - 4);
            }
            for (const std::wstring& name : lowercase_names) {
                if (candidate == name) {
                    found.push_back(entry.th32ProcessID);
                    break;
                }
            }
        } while (Process32NextW(snapshot.get(), &entry) != FALSE);
    }
    return found;
}

DWORD ParentProcessId(DWORD process_id) {
    if (process_id == 0) return 0;
    const Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid()) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD found = 0;
    if (Process32FirstW(snapshot.get(), &entry) != FALSE) {
        do {
            if (entry.th32ProcessID == process_id) {
                found = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot.get(), &entry) != FALSE);
    }
    return found;
}

/*
 * Optional foreground gate. Two-finger input only makes sense while the player
 * owns the screen: when Moonlight loses focus its window keeps rendering, but
 * the pointer now belongs to a local window, so forwarding contacts would move
 * the local cursor and click on whatever happens to be in front.
 *
 * Enabled with --gate <process>. Empty means "always forward", which is the
 * behaviour every earlier build had.
 */
class FocusGate {
public:
    explicit FocusGate(std::wstring process) : process_(Lowercase(std::move(process))) {
        if (process_.empty()) return;
        std::size_t start = 0;
        while (start <= process_.size()) {
            const std::size_t comma = process_.find(L',', start);
            std::wstring name = process_.substr(start, comma == std::wstring::npos
                                                           ? std::wstring::npos
                                                           : comma - start);
            while (!name.empty() && (name.back() == L' ' || name.back() == L'\t')) name.pop_back();
            while (!name.empty() && (name.front() == L' ' || name.front() == L'\t')) name.erase(0, 1);
            if (!name.empty()) names_.push_back(std::move(name));
            if (comma == std::wstring::npos) break;
            start = comma + 1;
        }
        if (names_.empty()) {
            process_.clear();
            return;
        }
        interactive_ = RunningOnInteractiveSession();
        std::cout << "focus gate armed for '";
        for (std::size_t index = 0; index < names_.size(); ++index) {
            if (index != 0) std::cout << ',';
            std::cout << Narrow(names_[index]);
        }
        std::cout << "' (session " << CurrentSessionId()
                  << (interactive_ ? ", interactive" : ", non-interactive: focus check disabled")
                  << ")\n";
        if (!interactive_) {
            std::cout << "warning: this process cannot see the user's desktop, so the gate "
                         "can only check that the player is running, not that it has focus\n";
        }
        allowed_ = Evaluate();
        std::cout << "focus gate is " << (allowed_ ? "open" : "closed") << " now\n";
        std::cout.flush();
    }

    [[nodiscard]] bool enabled() const noexcept { return !names_.empty(); }

    [[nodiscard]] bool allowed() {
        if (names_.empty()) return true;
        const ULONGLONG now = GetTickCount64();
        if (last_check_ != 0 && now - last_check_ < kPollMilliseconds) return allowed_;
        last_check_ = now;
        const bool value = Evaluate();
        if (value != allowed_) {
            allowed_ = value;
            std::cout << (value ? "focus gate open: player window is foreground\n"
                                : "focus gate closed: player window is not foreground\n");
            std::cout.flush();
        }
        return allowed_;
    }

private:
    static constexpr ULONGLONG kPollMilliseconds = 100;

    static DWORD CurrentSessionId() {
        DWORD session = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
        return session;
    }

    static bool RunningOnInteractiveSession() {
        const DWORD session = CurrentSessionId();
        return session != 0 && session == WTSGetActiveConsoleSessionId();
    }

    [[nodiscard]] bool Evaluate() const {
        const std::vector<DWORD> targets = FindProcessIds(names_);
        if (targets.empty()) return false;
        if (!interactive_) return true;
        const HWND foreground = GetForegroundWindow();
        if (foreground == nullptr) return false;
        DWORD owner = 0;
        GetWindowThreadProcessId(foreground, &owner);
        const auto owns = [&targets](DWORD id) {
            return std::find(targets.begin(), targets.end(), id) != targets.end();
        };
        if (owns(owner)) return true;
        /* Some players host the render window in a child process. */
        const DWORD parent = ParentProcessId(owner);
        return parent != 0 && owns(parent);
    }

    std::wstring process_;
    std::vector<std::wstring> names_;
    bool interactive_{false};
    bool allowed_{true};
    ULONGLONG last_check_{};
};

/*
 * Trace goes through WriteFile instead of fwrite. The CRT stream kept the data
 * in its own buffer, and a second process reading the file saw a zero-length
 * file until the receiver exited. FILE_SHARE_READ plus a periodic
 * FlushFileBuffers makes the log readable live.
 */
void WriteTraceLine(const Handle& trace, std::string_view line, std::size_t& rows) {
    if (!trace.valid() || rows > 20000) return;
    std::string row(line);
    row.push_back('\n');
    DWORD written = 0;
    WriteFile(trace.get(), row.data(), static_cast<DWORD>(row.size()), &written, nullptr);
    if ((++rows % 64u) == 0u) {
        FlushFileBuffers(trace.get());
    }
}

void Submit(const Handle& driver, const MTP_IOCTL_FRAME& frame) {
    if (!driver.valid()) {
        std::cout << "frame sequence=" << frame.sequence
                  << " active=" << static_cast<unsigned>(frame.active_contact_count)
                  << " reported=" << static_cast<unsigned>(frame.report_contact_count)
                  << " scan=" << frame.scan_time_100us << '\n';
        return;
    }
    DWORD returned = 0;
    if (!DeviceIoControl(driver.get(), IOCTL_MTP_SUBMIT_FRAME,
            const_cast<MTP_IOCTL_FRAME*>(&frame), sizeof(frame), nullptr, 0, &returned, nullptr)) {
        const DWORD error = GetLastError();
        std::cerr << "IOCTL_MTP_SUBMIT_FRAME failed: " << error
                  << " seq=" << frame.sequence
                  << " abi=" << frame.abi_version
                  << " flags=0x" << std::hex << frame.flags << std::dec
                  << " active=" << static_cast<unsigned>(frame.active_contact_count)
                  << " reported=" << static_cast<unsigned>(frame.report_contact_count)
                  << '\n';
        throw std::runtime_error("IOCTL_MTP_SUBMIT_FRAME failed: " + std::to_string(error));
    }
}

void PrintDriverStatus(const Handle& driver) {
    if (!driver.valid()) return;
    MTP_IOCTL_STATUS status{};
    DWORD returned = 0;
    if (DeviceIoControl(driver.get(), IOCTL_MTP_QUERY_STATUS, nullptr, 0,
                        &status, sizeof(status), &returned, nullptr)) {
        std::cout << "driver status: submits=" << status.submit_count
                  << " last_ntstatus=0x" << std::hex
                  << static_cast<std::uint32_t>(status.last_submit_status) << std::dec
                  << " report_bytes=" << status.last_report_size
                  << " active=" << static_cast<unsigned>(status.last_active_contact_count)
                  << " input_mode=" << static_cast<unsigned>(status.input_mode)
                  << " function=0x" << std::hex
                  << static_cast<unsigned>(status.function_switch) << std::dec
                  << " get_feature=" << status.get_feature_count
                  << " set_feature=" << status.set_feature_count << '\n';
    } else {
        std::cerr << "IOCTL_MTP_QUERY_STATUS failed: " << GetLastError() << '\n';
    }
}

SOCKET CreateServer(std::uint16_t port) {
    SOCKET server = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) throw std::runtime_error("socket failed");
    DWORD disabled = 0;
    setsockopt(server, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&disabled), sizeof(disabled));
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    address.sin6_port = htons(port);
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(server, 1) == SOCKET_ERROR) {
        closesocket(server);
        throw std::runtime_error("bind/listen failed: " + std::to_string(WSAGetLastError()));
    }
    return server;
}

SOCKET CreateControlServer() {
    SOCKET server = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server == INVALID_SOCKET) return INVALID_SOCKET;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(kControlPort);
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(server, 1) == SOCKET_ERROR) {
        closesocket(server);
        return INVALID_SOCKET;
    }
    return server;
}

/* Returns true once someone connected to the control port. The connection
   itself carries no payload; accept it and drop it. */
bool PollControl(SOCKET control) {
    if (control == INVALID_SOCKET) return false;
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(control, &readable);
    timeval immediate{0, 0};
    if (select(0, &readable, nullptr, nullptr, &immediate) != 1) return false;
    SOCKET accepted = accept(control, nullptr, nullptr);
    if (accepted != INVALID_SOCKET) closesocket(accepted);
    return true;
}

int SendStopRequest() {
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) return 1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(kControlPort);
    const int result = connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    closesocket(client);
    if (result == SOCKET_ERROR) {
        std::cout << "receiver is not running\n";
        return 0;
    }
    std::cout << "stop requested\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        WinsockRuntime winsock;

        std::uint16_t port = 39871;
        std::wstring gate_process;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument == "--stop") return SendStopRequest();
            if (argument == "--gate") {
                if (index + 1 >= argc) throw std::runtime_error("--gate needs a process name");
                gate_process = Widen(argv[++index]);
                if (gate_process.empty()) throw std::runtime_error("empty --gate process name");
            } else if (!argument.empty() && argument.front() == '-') {
                throw std::runtime_error("unknown option: " + std::string(argument));
            } else {
                port = static_cast<std::uint16_t>(std::stoul(std::string(argument)));
            }
        }

        FocusGate gate(gate_process);
        Handle trace(CreateFileW(
            L"C:\\temp\\mtp-trace.csv",
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        std::size_t trace_rows = 0;
        if (trace.valid()) {
            WriteTraceLine(trace, "seq,active,reported,scan_100us,button_flags,capture_us,x0,y0", trace_rows);
            FlushFileBuffers(trace.get());
        }

        Handle driver(CreateFileW(L"\\\\.\\MtpVhfTouchpad", GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!driver.valid()) {
            std::cout << "driver not present; parse/log mode\n";
        } else {
            PrintDriverStatus(driver);
        }

        SOCKET server = CreateServer(port);
        SOCKET control = CreateControlServer();
        std::cout << "listening on TCP " << port;
        if (control != INVALID_SOCKET) {
            std::cout << ", control on 127.0.0.1:" << kControlPort;
        }
        std::cout << '\n';

        bool stopping = false;
        for (;;) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(server, &readable);
            timeval idle{0, 200000};
            const int waiting = select(0, &readable, nullptr, nullptr, &idle);
            if (waiting == SOCKET_ERROR) throw std::runtime_error("select failed");
            if (PollControl(control)) {
                stopping = true;
                break;
            }
            if (waiting == 0) continue;

            SOCKET client = accept(server, nullptr, nullptr);
            if (client == INVALID_SOCKET) throw std::runtime_error("accept failed");
            DWORD nodelay = 1;
            setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
            std::cout << "client connected\n";
            mtp::StreamDecoder decoder;
            mtp::TouchSession session;
            if (trace.valid()) {
                session.SetTrace([&trace, &trace_rows](std::string_view line) {
                    WriteTraceLine(trace, line, trace_rows);
                });
            }
            std::array<std::uint8_t, 4096> buffer{};
            bool gate_released = false;
            try {
                for (;;) {
                    /* select only has to notice data or the idle timeout; it is
                       no longer driving any animation, so 100 ms is fine. */
                    fd_set readable_client;
                    FD_ZERO(&readable_client);
                    FD_SET(client, &readable_client);
                    timeval idle_client{0, 100000};
                    const int ready = select(0, &readable_client, nullptr, nullptr, &idle_client);
                    if (ready == SOCKET_ERROR) {
                        throw std::runtime_error("select failed");
                    }
                    if (PollControl(control)) {
                        stopping = true;
                        break;
                    }
                    if (ready == 0) {
                        if (!gate.allowed() && !gate_released) {
                            Submit(driver, session.ForceRelease());
                            gate_released = true;
                            std::cout << "focus gate closed; contacts released\n";
                        }
                        if (const auto release = session.OnTimeout(std::chrono::steady_clock::now())) {
                            if (gate.allowed()) Submit(driver, *release);
                        }
                        continue;
                    }
                    const int received = recv(client, reinterpret_cast<char*>(buffer.data()),
                                              static_cast<int>(buffer.size()), 0);
                    if (received == 0) break;
                    if (received < 0) {
                        throw std::runtime_error("recv failed");
                    }
                    decoder.Append(std::span<const std::uint8_t>(
                        buffer.data(), static_cast<std::size_t>(received)));
                    while (decoder.HasMessage()) {
                        const auto output = session.Process(decoder.PopMessage());
                        if (!output) continue;
                        if (gate.allowed()) {
                            gate_released = false;
                            Submit(driver, *output);
                        } else if (!gate_released) {
                            /* The driver keeps the last report, so a finger or a
                               button would stay latched while the gate is shut.
                               Release once, then keep consuming (and dropping)
                               frames so the MTP1 sequence stays continuous. */
                            Submit(driver, session.ForceRelease());
                            gate_released = true;
                            std::cout << "focus gate closed; contacts released\n";
                        }
                    }
                }
            } catch (const std::exception& error) {
                std::cerr << "connection rejected: " << error.what() << '\n';
            }
            if (const auto release = session.OnDisconnect()) Submit(driver, *release);
            if (trace.valid()) FlushFileBuffers(trace.get());
            PrintDriverStatus(driver);
            closesocket(client);
            std::cout << "client disconnected\n";
            if (stopping) break;
        }
        if (stopping) std::cout << "control port requested shutdown; contacts released\n";
        if (control != INVALID_SOCKET) closesocket(control);
        closesocket(server);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
