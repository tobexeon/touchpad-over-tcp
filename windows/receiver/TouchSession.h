#pragma once

#include "Mtp1.h"
#include "TouchpadIoctl.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mtp {

class TouchSession {
public:
    [[nodiscard]] std::optional<MTP_IOCTL_FRAME> Process(const Message& message);
    [[nodiscard]] std::optional<MTP_IOCTL_FRAME> OnDisconnect();
    [[nodiscard]] std::optional<MTP_IOCTL_FRAME> OnTimeout(std::chrono::steady_clock::time_point now);
    void SetTrace(std::function<void(std::string_view)> sink);
    [[nodiscard]] bool HasActiveContacts() const noexcept {
        return !active_contacts_.empty() || button_down_;
    }

    /*
     * Release every contact and every button immediately, without ending the
     * session. Needed whenever forwarding stops for a reason other than a
     * finger lift: the driver keeps the last report it was given, so a finger
     * or a button would stay latched until the next real frame arrives.
     */
    [[nodiscard]] MTP_IOCTL_FRAME ForceRelease() {
        return ReleaseAll(previous_sequence_, false);
    }

private:
    struct ActiveContact {
        std::uint16_t contact_id{};
        std::uint16_t last_logical_x{};
        std::uint16_t last_logical_y{};
        std::uint8_t last_flags{};
    };

    [[nodiscard]] MTP_IOCTL_FRAME ReleaseAll(std::uint32_t sequence, bool reset);
    [[nodiscard]] std::uint16_t NextScanTime(std::uint64_t capture_time_us);
    [[nodiscard]] std::uint16_t AllocateContactId(const std::unordered_set<std::uint16_t>& reserved_ids);
    [[nodiscard]] static std::uint16_t ToLogical(float value);
    [[nodiscard]] static std::uint16_t ToLogicalY(float value);

    bool hello_seen_{};
    bool reset_seen_{};
    bool sequence_seen_{};
    bool button_down_{};
    std::uint32_t previous_button_flags_{};
    std::uint32_t previous_sequence_{};

    // Insertion order is preserved so surviving contacts keep a stable position
    // in the report while always packing densely into slots [0 .. N-1].
    std::vector<std::pair<std::uint32_t, ActiveContact>> active_contacts_;
    // Contacts beyond the HID maximum are ignored for the lifetime of that
    // contact instead of tearing the connection down.
    std::unordered_set<std::uint32_t> suppressed_identifiers_;
    std::uint16_t next_contact_id_{};

    bool scan_time_seen_{};
    std::uint64_t last_capture_time_us_{};
    std::uint16_t last_scan_time_100us_{};
    std::chrono::steady_clock::time_point last_frame_time_{};
    std::function<void(std::string_view)> trace_{};
};

} // namespace mtp
