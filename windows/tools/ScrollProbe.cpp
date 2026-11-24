// Records system-generated pointer and wheel traffic so we can tell whether a
// two-finger pan is routed as a gesture (wheel) or as mousing (move).
//
// The recorder must run on the interactive desktop: Raw Input registered from
// an SSH/service session never sees the user's input queue.
//
// Usage:
//   scroll-probe <seconds> <output.csv>          record only
//   scroll-probe <seconds> <output.csv> --flick  record while injecting a
//                                                 synthetic two-finger pan

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace {

FILE* g_out = nullptr;
LARGE_INTEGER g_freq{};
LARGE_INTEGER g_start{};
std::map<HANDLE, std::string> g_names;

std::string NameFor(HANDLE device) {
    const auto found = g_names.find(device);
    if (found != g_names.end()) {
        return found->second;
    }
    std::string label = "unknown";
    UINT size = 0;
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &size) == 0 && size > 0) {
        std::wstring wide(size, L'\0');
        if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, wide.data(), &size) != static_cast<UINT>(-1)) {
            while (!wide.empty() && wide.back() == L'\0') {
                wide.pop_back();
            }
            label.clear();
            for (const wchar_t ch : wide) {
                label.push_back(ch < 128 ? static_cast<char>(ch) : '?');
            }
        }
    }
    if (label.find("VHF&COL01") != std::string::npos) {
        label = "MTP-TOUCHPAD";
    } else if (label.size() > 48) {
        label = label.substr(label.size() - 48);
    }
    g_names.emplace(device, label);
    return label;
}

long long NowUs() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return (now.QuadPart - g_start.QuadPart) * 1000000LL / g_freq.QuadPart;
}

void PumpMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void PutU16(unsigned char* out, std::uint16_t value) {
    out[0] = static_cast<unsigned char>(value >> 8);
    out[1] = static_cast<unsigned char>(value);
}

void PutU32(unsigned char* out, std::uint32_t value) {
    out[0] = static_cast<unsigned char>(value >> 24);
    out[1] = static_cast<unsigned char>(value >> 16);
    out[2] = static_cast<unsigned char>(value >> 8);
    out[3] = static_cast<unsigned char>(value);
}

void PutU64(unsigned char* out, std::uint64_t value) {
    for (int index = 0; index < 8; ++index) {
        out[index] = static_cast<unsigned char>(value >> (56 - 8 * index));
    }
}

void PutF32(unsigned char* out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    PutU32(out, bits);
}

constexpr std::uint32_t kMagic = 0x4D545031;
constexpr std::uint16_t kHello = 1;
constexpr std::uint16_t kFrame = 2;
constexpr std::uint16_t kReset = 3;
constexpr std::uint8_t kTip = 0x02;
constexpr std::uint8_t kInRange = 0x01;
constexpr std::uint8_t kConfidence = 0x04;

int BuildMessage(unsigned char* out, std::uint16_t type, std::uint32_t sequence,
                 std::uint64_t capture_us, std::uint16_t count, std::uint16_t flags,
                 const float* coordinates) {
    PutU32(out, kMagic);
    PutU16(out + 4, 1);
    PutU16(out + 6, type);
    PutU32(out + 8, static_cast<std::uint32_t>(count) * 44u);
    PutU32(out + 12, sequence);
    PutU16(out + 16, count);
    PutU16(out + 18, flags);
    PutU64(out + 20, capture_us);
    PutU64(out + 28, 0);
    unsigned char* cursor = out + 36;
    for (std::uint16_t index = 0; index < count; ++index) {
        PutU32(cursor, 100u + index);
        cursor[4] = 4;
        cursor[5] = static_cast<unsigned char>(kTip | kInRange | kConfidence);
        PutU16(cursor + 6, 0);
        PutF32(cursor + 8, coordinates[index * 2]);
        PutF32(cursor + 12, coordinates[index * 2 + 1]);
        PutF32(cursor + 16, 0.0f);
        PutF32(cursor + 20, 0.0f);
        PutF32(cursor + 24, 0.2f);
        PutF32(cursor + 28, 0.0f);
        PutF32(cursor + 32, 3.0f);
        PutF32(cursor + 36, 2.0f);
        PutF32(cursor + 40, 0.0f);
        cursor += 44;
    }
    return 36 + static_cast<int>(count) * 44;
}

SOCKET ConnectReceiver() {
    for (int attempt = 0; attempt < 15; ++attempt) {
        SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
            return INVALID_SOCKET;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(39871);
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            return sock;
        }
        closesocket(sock);
        Sleep(200);
    }
    return INVALID_SOCKET;
}

void SendFlick(SOCKET sock) {
    unsigned char buffer[64 + 5 * 44]{};
    float coordinates[10]{};
    std::uint32_t sequence = 1;
    std::uint64_t capture_us = static_cast<std::uint64_t>(NowUs()) * 10u;

    int length = BuildMessage(buffer, kHello, sequence++, capture_us, 0, 0, coordinates);
    send(sock, reinterpret_cast<const char*>(buffer), length, 0);
    length = BuildMessage(buffer, kReset, sequence++, capture_us, 0, 0, coordinates);
    send(sock, reinterpret_cast<const char*>(buffer), length, 0);

    const float step = 30000.0f * 0.008f / 32767.0f;
    float y = 0.60f;
    for (int index = 0; index < 30; ++index) {
        capture_us += 8000;
        y -= step;
        coordinates[0] = 0.40f;
        coordinates[1] = y;
        coordinates[2] = 0.52f;
        coordinates[3] = y;
        length = BuildMessage(buffer, kFrame, sequence++, capture_us, 2, 0, coordinates);
        send(sock, reinterpret_cast<const char*>(buffer), length, 0);
        Sleep(8);
        PumpMessages();
    }
    capture_us += 8000;
    length = BuildMessage(buffer, kFrame, sequence++, capture_us, 0, 0, coordinates);
    send(sock, reinterpret_cast<const char*>(buffer), length, 0);
    std::printf("flick sent, final y=%.3f\n", static_cast<double>(y));
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg != WM_INPUT) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    UINT size = 0;
    GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, nullptr, &size,
                    sizeof(RAWINPUTHEADER));
    if (size == 0 || size > 1024) {
        return 0;
    }
    alignas(8) static BYTE buffer[1024]{};
    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, buffer, &size,
                        sizeof(RAWINPUTHEADER)) != size) {
        return 0;
    }
    const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buffer);
    if (raw->header.dwType != RIM_TYPEMOUSE) {
        return 0;
    }
    const RAWMOUSE& mouse = raw->data.mouse;
    const bool has_wheel = (mouse.usButtonFlags & RI_MOUSE_WHEEL) != 0;
    const int wheel = has_wheel ? static_cast<int>(static_cast<SHORT>(mouse.usButtonData)) : 0;
    if (!has_wheel && mouse.lLastX == 0 && mouse.lLastY == 0) {
        return 0;
    }
    std::fprintf(g_out, "%lld,%s,%ld,%ld,%s,%d\n", NowUs(),
                 NameFor(raw->header.hDevice).c_str(),
                 static_cast<long>(mouse.lLastX), static_cast<long>(mouse.lLastY),
                 has_wheel ? "WHEEL" : "MOVE", wheel);
    std::fflush(g_out);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const int seconds = argc > 1 ? _wtoi(argv[1]) : 6;
    const wchar_t* path = argc > 2 ? argv[2] : L"scroll-probe.csv";
    bool flick = false;
    for (int index = 3; index < argc; ++index) {
        if (wcscmp(argv[index], L"--flick") == 0) {
            flick = true;
        }
    }
    if (seconds <= 0) {
        return 1;
    }
    if (_wfopen_s(&g_out, path, L"w") != 0 || g_out == nullptr) {
        std::fwprintf(stderr, L"cannot open %ls\n", path);
        return 1;
    }
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    std::fprintf(g_out, "time_us,device,dx,dy,kind,wheel\n");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MtpScrollProbe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"probe", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr) {
        return 1;
    }
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        std::fprintf(stderr, "RegisterRawInputDevices failed %lu\n", GetLastError());
        return 1;
    }

    if (flick) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return 1;
        }
        const SOCKET sock = ConnectReceiver();
        if (sock == INVALID_SOCKET) {
            std::fprintf(stderr, "receiver not reachable\n");
            std::fclose(g_out);
            WSACleanup();
            return 1;
        }
        SendFlick(sock);
    }

    const LONGLONG deadline = NowUs() + static_cast<LONGLONG>(seconds) * 1000000LL;
    while (NowUs() < deadline) {
        PumpMessages();
        Sleep(1);
    }
    if (flick) {
        WSACleanup();
    }
    std::fclose(g_out);
    std::printf("probe done\n");
    return 0;
}
