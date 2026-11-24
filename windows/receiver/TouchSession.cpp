#include "TouchSession.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mtp {
namespace {
constexpr std::uint8_t kTip = 0x02;
constexpr std::uint8_t kConfidence = 0x04;
/* A dropped link must not impersonate a finger lift; keep this well above any
   realistic transport hiccup. */
constexpr auto kFrameTimeout = std::chrono::milliseconds(1500);
constexpr std::uint16_t kContactIdModulus = 256u;
} // namespace

std::optional<MTP_IOCTL_FRAME> TouchSession::Process(const Message& message) {
    if (sequence_seen_ && message.sequence != previous_sequence_ + 1u) {
        throw ProtocolError("sequence discontinuity");
    }
    sequence_seen_ = true;
    previous_sequence_ = message.sequence;

    if (message.type == MessageType::Hello) {
        if (hello_seen_ || reset_seen_) {
            throw ProtocolError("unexpected HELLO");
        }
        hello_seen_ = true;
        return std::nullopt;
    }
    if (!hello_seen_) {
        throw ProtocolError("message received before HELLO");
    }
    if (message.type == MessageType::Reset) {
        reset_seen_ = true;
        return ReleaseAll(message.sequence, true);
    }
    if (!reset_seen_) {
        throw ProtocolError("FRAME received before RESET");
    }

    std::unordered_map<std::uint32_t, const Contact*> present_tips;
    for (const Contact& contact : message.contacts) {
        if ((contact.flags & kTip) != 0) {
            present_tips.emplace(contact.identifier, &contact);
        }
    }

    for (auto it = suppressed_identifiers_.begin(); it != suppressed_identifiers_.end();) {
        if (!present_tips.contains(*it)) {
            it = suppressed_identifiers_.erase(it);
        } else {
            ++it;
        }
    }

    MTP_IOCTL_FRAME output{};
    output.abi_version = MTP_IOCTL_ABI_VERSION;
    output.sequence = message.sequence;
    output.flags = MTP_IOCTL_FRAME_HAS_SCAN_TIME;
    if ((message.flags & kFrameButton) != 0) output.flags |= MTP_IOCTL_FRAME_BUTTON;
    if ((message.flags & kFrameButton2) != 0) output.flags |= MTP_IOCTL_FRAME_BUTTON2;

    const std::uint32_t button_flags =
        output.flags & (MTP_IOCTL_FRAME_BUTTON | MTP_IOCTL_FRAME_BUTTON2);
    const bool button_changed = button_flags != previous_button_flags_;
    button_down_ = button_flags != 0;
    previous_button_flags_ = button_flags;

    std::unordered_set<std::uint16_t> used_ids_in_frame;
    for (const auto& entry : active_contacts_) {
        used_ids_in_frame.insert(entry.second.contact_id);
    }

    // 1) Leaving contacts first: Tip=0, and the coordinates must be exactly the
    //    ones from their last Tip=1 frame.
    std::vector<std::pair<std::uint32_t, ActiveContact>> remaining;
    remaining.reserve(active_contacts_.size());
    for (const auto& entry : active_contacts_) {
        if (!present_tips.contains(entry.first)) {
            if (output.report_contact_count < MTP_HID_MAX_CONTACTS) {
                const std::uint8_t slot = output.report_contact_count++;
                MTP_IOCTL_CONTACT& target = output.contacts[slot];
                target.slot = slot;
                target.contact_id = entry.second.contact_id;
                target.flags = static_cast<std::uint8_t>(
                    (entry.second.last_flags & static_cast<std::uint8_t>(~kTip)) | kConfidence);
                target.x = entry.second.last_logical_x;
                target.y = entry.second.last_logical_y;
            }
        } else {
            remaining.push_back(entry);
        }
    }

    // 2) Surviving contacts, densely packed after the lifts.
    for (auto& entry : remaining) {
        const Contact* source = present_tips.at(entry.first);
        const std::uint16_t logical_x = ToLogical(source->x);
        const std::uint16_t logical_y = ToLogicalY(source->y);
        const std::uint8_t logical_flags =
            static_cast<std::uint8_t>(source->flags | kConfidence);
        if (output.report_contact_count < MTP_HID_MAX_CONTACTS) {
            const std::uint8_t slot = output.report_contact_count++;
            MTP_IOCTL_CONTACT& target = output.contacts[slot];
            target.slot = slot;
            target.contact_id = entry.second.contact_id;
            target.flags = logical_flags;
            target.x = logical_x;
            target.y = logical_y;
            ++output.active_contact_count;
            entry.second.last_logical_x = logical_x;
            entry.second.last_logical_y = logical_y;
            entry.second.last_flags = logical_flags;
        }
    }

    // 3) Admit newly landed contacts while capacity remains.
    for (const Contact& contact : message.contacts) {
        if ((contact.flags & kTip) == 0) {
            continue;
        }
        const bool already_active = std::any_of(remaining.begin(), remaining.end(),
            [id = contact.identifier](const auto& item) { return item.first == id; });
        if (already_active || suppressed_identifiers_.contains(contact.identifier)) {
            continue;
        }
        if (output.report_contact_count >= MTP_HID_MAX_CONTACTS ||
            remaining.size() >= MTP_HID_MAX_CONTACTS) {
            suppressed_identifiers_.insert(contact.identifier);
            continue;
        }
        const std::uint16_t contact_id = AllocateContactId(used_ids_in_frame);
        used_ids_in_frame.insert(contact_id);

        const std::uint16_t logical_x = ToLogical(contact.x);
        const std::uint16_t logical_y = ToLogicalY(contact.y);
        const std::uint8_t logical_flags =
            static_cast<std::uint8_t>(contact.flags | kConfidence);
        const std::uint8_t slot = output.report_contact_count++;
        MTP_IOCTL_CONTACT& target = output.contacts[slot];
        target.slot = slot;
        target.contact_id = contact_id;
        target.flags = logical_flags;
        target.x = logical_x;
        target.y = logical_y;
        ++output.active_contact_count;

        ActiveContact state{};
        state.contact_id = contact_id;
        state.last_logical_x = logical_x;
        state.last_logical_y = logical_y;
        state.last_flags = logical_flags;
        remaining.push_back({contact.identifier, state});
    }

    active_contacts_ = std::move(remaining);
    last_frame_time_ = std::chrono::steady_clock::now();

    // Nothing lifted, nothing landed, button unchanged: an empty report would
    // only be noise on the wire and in the HID stream.
    if (output.report_contact_count == 0 && !button_down_ && !button_changed) {
        return std::nullopt;
    }

    output.scan_time_100us = NextScanTime(message.capture_time_us);

    if (trace_) {
        /* capture_time_us and the first contact position are here because a
           double click is decided by two things the HID report hides: how long
           the button stayed up between presses, and how far apart the two press
           positions were. Both are recoverable from these columns. */
        char line[160];
        const std::uint16_t first_x =
            output.report_contact_count != 0 ? output.contacts[0].x : 0;
        const std::uint16_t first_y =
            output.report_contact_count != 0 ? output.contacts[0].y : 0;
        std::snprintf(line, sizeof(line), "%u,%u,%u,%u,0x%x,%llu,%u,%u",
                      output.sequence,
                      static_cast<unsigned>(output.active_contact_count),
                      static_cast<unsigned>(output.report_contact_count),
                      static_cast<unsigned>(output.scan_time_100us),
                      static_cast<unsigned>(button_flags),
                      static_cast<unsigned long long>(message.capture_time_us),
                      static_cast<unsigned>(first_x),
                      static_cast<unsigned>(first_y));
        trace_(line);
    }
    return output;
}

std::optional<MTP_IOCTL_FRAME> TouchSession::OnDisconnect() {
    const bool should_report = HasActiveContacts();
    MTP_IOCTL_FRAME release = ReleaseAll(previous_sequence_, true);
    hello_seen_ = false;
    reset_seen_ = false;
    sequence_seen_ = false;
    scan_time_seen_ = false;
    return should_report ? std::optional<MTP_IOCTL_FRAME>(release) : std::nullopt;
}

std::optional<MTP_IOCTL_FRAME> TouchSession::OnTimeout(std::chrono::steady_clock::time_point now) {
    if (!HasActiveContacts() || now - last_frame_time_ < kFrameTimeout) {
        return std::nullopt;
    }
    return ReleaseAll(previous_sequence_, true);
}

MTP_IOCTL_FRAME TouchSession::ReleaseAll(std::uint32_t sequence, bool reset) {
    MTP_IOCTL_FRAME output{};
    output.abi_version = MTP_IOCTL_ABI_VERSION;
    output.sequence = sequence;
    output.flags = (reset ? MTP_IOCTL_FRAME_RESET : 0u) | MTP_IOCTL_FRAME_HAS_SCAN_TIME;
    output.scan_time_100us = NextScanTime(last_capture_time_us_ + 8000ULL);
    for (const auto& entry : active_contacts_) {
        if (output.report_contact_count >= MTP_HID_MAX_CONTACTS) {
            break;
        }
        const std::uint8_t slot = output.report_contact_count++;
        MTP_IOCTL_CONTACT& target = output.contacts[slot];
        target.slot = slot;
        target.contact_id = entry.second.contact_id;
        target.flags = static_cast<std::uint8_t>(
            (entry.second.last_flags & static_cast<std::uint8_t>(~kTip)) | kConfidence);
        target.x = entry.second.last_logical_x;
        target.y = entry.second.last_logical_y;
    }
    active_contacts_.clear();
    suppressed_identifiers_.clear();
    button_down_ = false;
    previous_button_flags_ = 0;
    return output;
}

std::uint16_t TouchSession::NextScanTime(std::uint64_t capture_time_us) {
    std::uint16_t scan = static_cast<std::uint16_t>((capture_time_us / 100u) & 0xffffu);
    /* Two reports can share a capture stamp (a button event fires between two
       contact callbacks). The host uses Scan Time to order and time-stamp
       samples, so an exactly repeated value has to be nudged forward. */
    if (scan_time_seen_ &&
        capture_time_us >= last_capture_time_us_ &&
        (capture_time_us - last_capture_time_us_) < 3000000ULL &&
        scan == last_scan_time_100us_) {
        scan = static_cast<std::uint16_t>((scan + 1u) & 0xffffu);
    }
    scan_time_seen_ = true;
    last_capture_time_us_ = capture_time_us;
    last_scan_time_100us_ = scan;
    return scan;
}

std::uint16_t TouchSession::AllocateContactId(
    const std::unordered_set<std::uint16_t>& reserved_ids) {
    for (std::uint16_t offset = 0; offset < kContactIdModulus; ++offset) {
        const auto candidate = static_cast<std::uint16_t>(
            (next_contact_id_ + offset) % kContactIdModulus);
        if (!reserved_ids.contains(candidate)) {
            next_contact_id_ = static_cast<std::uint16_t>(
                (candidate + 1u) % kContactIdModulus);
            return candidate;
        }
    }
    throw ProtocolError("no free HID contact id");
}

std::uint16_t TouchSession::ToLogical(float value) {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint16_t>(std::lround(clamped * MTP_COORDINATE_LOGICAL_MAX));
}

std::uint16_t TouchSession::ToLogicalY(float value) {
    // MultitouchSupport grows Y upward from the bottom edge. Windows Precision
    // Touchpad coordinates grow downward from the top edge.
    return ToLogical(1.0f - value);
}

void TouchSession::SetTrace(std::function<void(std::string_view)> sink) {
    trace_ = std::move(sink);
}

} // namespace mtp
