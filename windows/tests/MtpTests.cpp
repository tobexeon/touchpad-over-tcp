#include "Mtp1.h"
#include "TouchSession.h"

#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Check(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void PutU16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void PutU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

void PutU64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    PutU32(bytes, offset, static_cast<std::uint32_t>(value >> 32));
    PutU32(bytes, offset + 4, static_cast<std::uint32_t>(value));
}

void PutF32(std::vector<std::uint8_t>& bytes, std::size_t offset, float value) {
    PutU32(bytes, offset, std::bit_cast<std::uint32_t>(value));
}

struct WireContact { std::uint32_t id; std::uint8_t flags; float x; float y; };

std::vector<std::uint8_t> Encode(mtp::MessageType type, std::uint32_t sequence,
                                 std::span<const WireContact> contacts = {},
                                 std::uint16_t flags = 0,
                                 std::uint64_t capture_us = 0) {
    const std::size_t count = type == mtp::MessageType::Frame ? contacts.size() : 0;
    std::vector<std::uint8_t> bytes(mtp::kHeaderSize + count * mtp::kContactSize, 0);
    bytes[0] = 'M'; bytes[1] = 'T'; bytes[2] = 'P'; bytes[3] = '1';
    PutU16(bytes, 4, 1);
    PutU16(bytes, 6, static_cast<std::uint16_t>(type));
    PutU32(bytes, 8, static_cast<std::uint32_t>(count * mtp::kContactSize));
    PutU32(bytes, 12, sequence);
    PutU16(bytes, 16, static_cast<std::uint16_t>(count));
    PutU16(bytes, 18, flags);
    PutU64(bytes, 20, capture_us != 0 ? capture_us : 1000000ULL + sequence * 8000ULL);
    PutU64(bytes, 28, 2000000ULL + sequence * 8000ULL);
    for (std::size_t index = 0; index < count; ++index) {
        const std::size_t offset = mtp::kHeaderSize + index * mtp::kContactSize;
        PutU32(bytes, offset, contacts[index].id);
        bytes[offset + 4] = 4;
        bytes[offset + 5] = contacts[index].flags;
        PutF32(bytes, offset + 8, contacts[index].x);
        PutF32(bytes, offset + 12, contacts[index].y);
    }
    return bytes;
}

void ExpectProtocolError(const std::function<void()>& operation) {
    try { operation(); } catch (const mtp::ProtocolError&) { return; }
    throw std::runtime_error("expected ProtocolError");
}

void TestFragmentationAndCoalescing() {
    const auto hello = Encode(mtp::MessageType::Hello, 1);
    const std::array contacts{WireContact{42, 7, 0.25f, 0.75f}};
    const auto frame = Encode(mtp::MessageType::Frame, 2, contacts);
    std::vector<std::uint8_t> stream = hello;
    stream.insert(stream.end(), frame.begin(), frame.end());
    for (std::size_t split = 0; split <= stream.size(); ++split) {
        mtp::StreamDecoder decoder;
        decoder.Append(std::span(stream).first(split));
        decoder.Append(std::span(stream).subspan(split));
        Check(decoder.HasMessage(), "HELLO missing after fragmentation");
        Check(decoder.PopMessage().type == mtp::MessageType::Hello, "wrong first message");
        Check(decoder.HasMessage(), "FRAME missing after fragmentation");
        const auto decoded = decoder.PopMessage();
        Check(decoded.contacts.size() == 1 && decoded.contacts[0].identifier == 42, "contact mismatch");
        Check(std::abs(decoded.contacts[0].x - 0.25f) < 0.00001f, "float mismatch");
    }
}

void TestCoalescedBurstDoesNotReject() {
    mtp::StreamDecoder decoder;
    std::vector<std::uint8_t> burst;
    constexpr std::uint32_t kFrames = 32;
    const std::array contacts{WireContact{1, 7, 0.25f, 0.5f}};
    for (std::uint32_t seq = 0; seq < kFrames; ++seq) {
        const auto frame = Encode(mtp::MessageType::Frame, seq, contacts);
        burst.insert(burst.end(), frame.begin(), frame.end());
    }
    Check(burst.size() > mtp::kMaxMessageSize * 2,
          "burst must exceed the old append guard to be a real regression test");
    decoder.Append(burst);
    std::uint32_t decoded = 0;
    while (decoder.HasMessage()) {
        Check(decoder.PopMessage().type == mtp::MessageType::Frame, "burst frame type mismatch");
        ++decoded;
    }
    Check(decoded == kFrames, "coalesced burst frames were not all decoded");
}

void TestMalformedInput() {
    auto bytes = Encode(mtp::MessageType::Frame, 1, std::array{WireContact{1, 7, 0.5f, 0.5f}});
    bytes[0] = 'X';
    ExpectProtocolError([&] { (void)mtp::DecodeMessage(bytes); });
    bytes = Encode(mtp::MessageType::Frame, 1, std::array{WireContact{1, 7, 0.5f, 0.5f}});
    PutF32(bytes, mtp::kHeaderSize + 8, std::numeric_limits<float>::quiet_NaN());
    ExpectProtocolError([&] { (void)mtp::DecodeMessage(bytes); });
    const std::array duplicates{WireContact{7, 7, 0.1f, 0.2f}, WireContact{7, 7, 0.3f, 0.4f}};
    bytes = Encode(mtp::MessageType::Frame, 1, duplicates);
    ExpectProtocolError([&] { (void)mtp::DecodeMessage(bytes); });
    bytes = Encode(mtp::MessageType::Hello, 1);
    PutU16(bytes, 18, mtp::kFrameButton);
    ExpectProtocolError([&] { (void)mtp::DecodeMessage(bytes); });
    bytes = Encode(mtp::MessageType::Frame, 1);
    PutU16(bytes, 18, 0x8000);
    ExpectProtocolError([&] { (void)mtp::DecodeMessage(bytes); });
}

void TestLifecycleSlotsAndLift() {
    mtp::TouchSession session;
    Check(!session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Hello, 10))), "HELLO reported");
    const auto reset = session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Reset, 11)));
    Check(reset && reset->flags == (MTP_IOCTL_FRAME_RESET | MTP_IOCTL_FRAME_HAS_SCAN_TIME),
          "RESET not emitted");

    const std::array first{WireContact{100, 7, -1.0f, 2.0f}, WireContact{200, 7, 0.5f, 0.25f}};
    const auto report1 = session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Frame, 12, first)));
    Check(report1 && report1->active_contact_count == 2 && report1->report_contact_count == 2,
          "active/report count mismatch");
    Check(report1->contacts[0].slot == 0 && report1->contacts[0].x == 0 &&
          report1->contacts[0].y == 0, "slot/clamp/inverted-y mismatch");
    Check(report1->contacts[1].slot == 1, "second slot mismatch");
    Check(report1->contacts[1].y ==
              static_cast<std::uint16_t>(std::lround(0.75f * MTP_COORDINATE_LOGICAL_MAX)),
          "Y axis was not inverted");
    Check(report1->contacts[0].flags & MTP_IOCTL_CONTACT_CONFIDENCE,
          "active contact did not report Confidence");
    const std::uint16_t id100 = report1->contacts[0].contact_id;
    const std::uint16_t id200 = report1->contacts[1].contact_id;

    const std::array second{WireContact{200, 7, 0.6f, 0.3f}};
    const auto report2 = session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Frame, 13, second)));
    Check(report2 && report2->active_contact_count == 1 && report2->report_contact_count == 2,
          "lift report mismatch");
    Check((report2->contacts[0].flags & MTP_IOCTL_CONTACT_TIP) == 0, "lift Tip remained set");
    Check(report2->contacts[0].flags & MTP_IOCTL_CONTACT_CONFIDENCE,
          "lift contact did not report Confidence");
    Check(report2->contacts[0].contact_id == id100, "lift contact id changed");
    Check(report2->contacts[0].x == report1->contacts[0].x &&
          report2->contacts[0].y == report1->contacts[0].y,
          "Tip=0 lift coordinates changed");
    Check(report2->contacts[1].slot == 1 && report2->contacts[1].contact_id == id200,
          "surviving contact in lift frame invalid");

    // Next frame carries only Finger 200: it MUST compact into slot 0.
    const std::array second_only{WireContact{200, 7, 0.65f, 0.35f}};
    const auto report2b = session.Process(mtp::DecodeMessage(
        Encode(mtp::MessageType::Frame, 14, second_only)));
    Check(report2b && report2b->active_contact_count == 1 && report2b->report_contact_count == 1,
          "single survivor count mismatch");
    Check(report2b->contacts[0].slot == 0 && report2b->contacts[0].contact_id == id200,
          "surviving contact stayed sparse instead of packing into slot 0");

    const std::array third{WireContact{300, 7, 0.2f, 0.2f}, WireContact{200, 7, 0.7f, 0.4f}};
    const auto report3 = session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Frame, 15, third)));
    Check(report3 && report3->active_contact_count == 2 && report3->report_contact_count == 2,
          "third report active count mismatch");
    Check(report3->contacts[0].slot == 0 && report3->contacts[1].slot == 1,
          "collection positions are not dense");
    Check(report3->contacts[0].contact_id != report3->contacts[1].contact_id,
          "two live contacts share one contact id");
    const auto pressed = session.Process(
        mtp::DecodeMessage(Encode(mtp::MessageType::Frame, 16, third, mtp::kFrameButton)));
    Check(pressed && (pressed->flags & MTP_IOCTL_FRAME_BUTTON) != 0,
          "button state was not handed to the driver");
}

void TestTwoFingerSequentialLiftPreservesCleanLiftPackets() {
    mtp::TouchSession session;
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Hello, 1)));
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Reset, 2)));
    std::uint64_t capture = 1000000;
    std::uint32_t sequence = 3;
    float y = 0.60f;
    for (int i = 0; i < 15; ++i) {
        capture += 8000;
        y -= 0.02f;
        const std::array contacts{WireContact{11, 7, 0.40f, y}, WireContact{12, 7, 0.52f, y}};
        const auto rep = session.Process(mtp::DecodeMessage(
            Encode(mtp::MessageType::Frame, sequence++, contacts, 0, capture)));
        Check(rep && rep->active_contact_count == 2 && rep->report_contact_count == 2,
              "pan frame lost");
    }

    capture += 8000;
    const std::array one_left{WireContact{12, 7, 0.52f, y - 0.01f}};
    const auto lift1 = session.Process(mtp::DecodeMessage(
        Encode(mtp::MessageType::Frame, sequence++, one_left, 0, capture)));
    Check(lift1 && lift1->report_contact_count == 2 && lift1->active_contact_count == 1,
          "first lift must keep report_contact_count=2 so Tip=0 is delivered");
    const std::uint16_t id12 = lift1->contacts[1].contact_id;

    capture += 8000;
    const std::array one_still{WireContact{12, 7, 0.52f, y - 0.02f}};
    const auto mid = session.Process(mtp::DecodeMessage(
        Encode(mtp::MessageType::Frame, sequence++, one_still, 0, capture)));
    Check(mid && mid->report_contact_count == 1 && mid->active_contact_count == 1 &&
          mid->contacts[0].slot == 0 && mid->contacts[0].contact_id == id12,
          "remaining finger lost or placed in a sparse slot");

    capture += 8000;
    const auto lift2 = session.Process(mtp::DecodeMessage(
        Encode(mtp::MessageType::Frame, sequence++, {}, 0, capture)));
    Check(lift2 && lift2->report_contact_count == 1 && lift2->active_contact_count == 0,
          "final lift must report the Tip=0 contact with report_contact_count=1");
    Check((lift2->contacts[0].flags & MTP_IOCTL_CONTACT_TIP) == 0 &&
          lift2->contacts[0].contact_id == id12 &&
          lift2->contacts[0].x == mid->contacts[0].x &&
          lift2->contacts[0].y == mid->contacts[0].y,
          "final lift did not preserve the exact Tip=0 id and coordinates");

    capture += 8000;
    const auto idle = session.Process(mtp::DecodeMessage(
        Encode(mtp::MessageType::Frame, sequence++, {}, 0, capture)));
    Check(!idle.has_value(), "redundant 0->0 empty frame should not be submitted");
}

void TestSixthContactSuppressedWithoutDisconnect() {
    mtp::TouchSession session;
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Hello, 1)));
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Reset, 2)));
    const std::array six{
        WireContact{1, 7, 0.1f, 0.2f}, WireContact{2, 7, 0.2f, 0.3f},
        WireContact{3, 7, 0.3f, 0.4f}, WireContact{4, 7, 0.4f, 0.5f},
        WireContact{5, 7, 0.5f, 0.6f}, WireContact{6, 7, 0.6f, 0.7f}};
    const auto report = session.Process(mtp::DecodeMessage(
        Encode(mtp::MessageType::Frame, 3, six)));
    Check(report && report->report_contact_count == 5 && report->active_contact_count == 5,
          "sixth contact was not clamped to the HID maximum");
}

void TestScanTimeAdvancesAndDoesNotDuplicate() {
    mtp::TouchSession session;
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Hello, 1)));
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Reset, 2)));
    const std::array c{WireContact{1, 7, 0.5f, 0.5f}};
    // 6553600 / 100 == 65536, which wraps to 0 on the 16-bit Scan Time field.
    const auto f1 = session.Process(mtp::DecodeMessage(
        Encode(mtp::MessageType::Frame, 3, c, 0, 6553600ULL)));
    const auto f2 = session.Process(mtp::DecodeMessage(
        Encode(mtp::MessageType::Frame, 4, c, mtp::kFrameButton, 6553600ULL)));
    Check(f1 && f2, "missing scan time frames");
    Check((f1->flags & MTP_IOCTL_FRAME_HAS_SCAN_TIME) != 0 && f1->scan_time_100us == 0,
          "ScanTime wrap-around to 0 not reported with HAS_SCAN_TIME");
    Check(f2->scan_time_100us != f1->scan_time_100us,
          "duplicate ScanTime emitted on consecutive frames");
}

void TestContactIdRotationAcrossTaps() {
    mtp::TouchSession session;
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Hello, 1)));
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Reset, 2)));
    std::uint32_t sequence = 3;
    std::array<std::uint16_t, 5> ids{};
    for (std::size_t tap = 0; tap < ids.size(); ++tap) {
        const auto down = session.Process(mtp::DecodeMessage(Encode(
            mtp::MessageType::Frame, sequence++, std::array{WireContact{100, 7, 0.1f, 0.1f}})));
        Check(down && down->active_contact_count == 1, "tap down missing contact");
        Check(down->contacts[0].slot == 0, "single touch must occupy collection 0");
        ids[tap] = down->contacts[0].contact_id;
        const auto up = session.Process(mtp::DecodeMessage(
            Encode(mtp::MessageType::Frame, sequence++)));
        Check(up && up->active_contact_count == 0 && up->report_contact_count == 1,
              "tap lift not reported");
    }
    for (std::size_t i = 1; i < ids.size(); ++i) {
        Check(ids[i] != ids[i - 1], "consecutive touches reused the same contact id");
    }
}

void TestSequenceAndDisconnectRelease() {
    mtp::TouchSession session;
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Hello, 1)));
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Reset, 2)));
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Frame, 3,
        std::array{WireContact{1, 7, 0.5f, 0.5f}})));
    ExpectProtocolError([&] {
        (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Frame, 5)));
    });
    const auto release = session.OnDisconnect();
    Check(release && release->report_contact_count == 1 && release->active_contact_count == 0,
          "disconnect did not release the contact");
}

void TestButtonOnlyDisconnectRelease() {
    mtp::TouchSession session;
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Hello, 1)));
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Reset, 2)));
    const auto pressed = session.Process(
        mtp::DecodeMessage(Encode(mtp::MessageType::Frame, 3, {}, mtp::kFrameButton)));
    Check(pressed && (pressed->flags & MTP_IOCTL_FRAME_BUTTON) != 0,
          "button-only press missing");
    const auto release = session.OnDisconnect();
    Check(release && (release->flags & MTP_IOCTL_FRAME_BUTTON) == 0,
          "disconnect did not release the button");
}

void TestButton2RightClick() {
    mtp::TouchSession session;
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Hello, 1)));
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Reset, 2)));
    const auto pressed = session.Process(mtp::DecodeMessage(
        Encode(mtp::MessageType::Frame, 3, {}, mtp::kFrameButton2)));
    Check(pressed && (pressed->flags & MTP_IOCTL_FRAME_BUTTON2) != 0,
          "button2 (right click) was not handed to the driver");
    Check((pressed->flags & MTP_IOCTL_FRAME_BUTTON) == 0, "button2 must not set button1");
    const auto release = session.OnDisconnect();
    Check(release && (release->flags & MTP_IOCTL_FRAME_BUTTON2) == 0,
          "disconnect did not release button2");
}

void TestSustainedRate() {
    mtp::TouchSession session;
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Hello, 1)));
    (void)session.Process(mtp::DecodeMessage(Encode(mtp::MessageType::Reset, 2)));
    const std::array contacts{WireContact{1, 7, 0.1f, 0.2f}, WireContact{2, 7, 0.2f, 0.3f},
                              WireContact{3, 7, 0.3f, 0.4f}, WireContact{4, 7, 0.4f, 0.5f},
                              WireContact{5, 7, 0.5f, 0.6f}};
    for (std::uint32_t sequence = 3; sequence < 12503; ++sequence) {
        const auto report = session.Process(mtp::DecodeMessage(
            Encode(mtp::MessageType::Frame, sequence, contacts)));
        Check(report && report->active_contact_count == 5, "sustained report failed");
    }
}

} // namespace

int main() {
    try {
        TestFragmentationAndCoalescing();
        TestCoalescedBurstDoesNotReject();
        TestMalformedInput();
        TestLifecycleSlotsAndLift();
        TestTwoFingerSequentialLiftPreservesCleanLiftPackets();
        TestSixthContactSuppressedWithoutDisconnect();
        TestScanTimeAdvancesAndDoesNotDuplicate();
        TestContactIdRotationAcrossTaps();
        TestSequenceAndDisconnectRelease();
        TestButtonOnlyDisconnectRelease();
        TestButton2RightClick();
        TestSustainedRate();
        std::cout << "mtp-tests: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "mtp-tests: failed: " << error.what() << '\n';
        return 1;
    }
}
