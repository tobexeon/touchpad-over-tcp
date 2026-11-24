#include "Mtp1.h"
#include "TouchSession.h"
#include "TouchpadIoctl.h"

#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

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

} // namespace

int main(int argc, char** argv) {
    try {
        const std::uint16_t port = argc > 1
            ? static_cast<std::uint16_t>(std::stoul(argv[1])) : 39871;
        WinsockRuntime winsock;
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
            WriteTraceLine(trace, "seq,active,reported,scan_100us,button_flags", trace_rows);
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
        std::cout << "listening on TCP " << port << '\n';
        for (;;) {
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
            try {
                for (;;) {
                    /* select only has to notice data or the idle timeout; it is
                       no longer driving any animation, so 100 ms is fine. */
                    fd_set readable;
                    FD_ZERO(&readable);
                    FD_SET(client, &readable);
                    timeval idle{0, 100000};
                    const int ready = select(0, &readable, nullptr, nullptr, &idle);
                    if (ready == SOCKET_ERROR) {
                        throw std::runtime_error("select failed");
                    }
                    if (ready == 0) {
                        if (const auto release = session.OnTimeout(std::chrono::steady_clock::now())) {
                            Submit(driver, *release);
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
                        if (output) Submit(driver, *output);
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
        }
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    }
}
