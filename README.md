# touchpad-over-tcp

Use a MacBook's built-in trackpad as a Windows Precision Touchpad.

The Mac captures raw contacts from `MultitouchSupport.framework` and streams them over TCP. A
Windows receiver maps them to a fixed IOCTL structure, and a KMDF + Virtual HID Framework driver
publishes them as a Precision Touchpad device. Windows handles the pointer, two-finger scrolling,
inertia and gestures itself, so nothing is emulated with mouse events.

This is a fork of [fakebob2004/touchpad](https://github.com/fakebob2004/touchpad). The report
semantics were corrected here; see [Changes](#changes).

## Status

Developed and tested against a MacBook Pro 14 (M1 Pro) and Windows 11 26100.

- Pointer, two-finger scroll, native inertia: working
- Pinch zoom: working
- Physical click (Button 1): working, triggered by pressure
- Two-finger force press (Button 3, right click): working
- Three- and four-finger gestures: not tested
- Pairing, authentication, encryption: not implemented

## How it works

```text
MacBook trackpad
  -> MultitouchSupport.framework        ~125 Hz contact callbacks
  -> mac-touch-agent                    MTP1 over TCP, port 39871
  -> mtp-receiver.exe                   contact mapping, fixed IOCTL
  -> MtpVhfTouchpad.sys (KMDF + VHF)    HID input reports
  -> Windows Precision Touchpad stack
```

The device exposes exactly two HID top-level collections: the Touch Pad collection and the
Configuration collection. No mouse collection is present, so the trackpad never appears as an
additional mouse.

## Changes from upstream

- `Contact Count` now reports every contact in the frame, including the `Tip=0` lift contacts.
  Upstream reported the active count, so a lift frame carried `ContactCount = 0`, Windows skipped
  the whole `Contacts[]` array and never saw the finger leave. Gestures were then cancelled by the
  system timeout instead of ending normally, which also removed native inertia.
- Contacts are packed into `Contacts[0 .. ContactCount-1]`. Upstream wrote them at the contact slot
  index, which left zero-filled holes that Windows read as phantom fingers.
- All device-side momentum code was removed. The inertia you feel is the Windows one.
- Report descriptor: `Button 1..3` (right click is Button 3), `Contact Identifier` logical maximum
  `0x7FFFFFFF`, physical size 12.48 x 7.68 cm, `Scan Time` units reset after the field.
- `Scan Time` is absolute and monotonic, never repeated between adjacent frames.
- Two-finger force press is judged on the mean tip pressure with a leaky two-frame arm. The previous
  minimum-of-two test missed about 44% of fast presses on the measured panel, because two fingers of
  one hand routinely differ in reported pressure by 2x.
- The sixth contact is ignored instead of dropping the TCP session.

## Requirements

macOS (trackpad side):

- Apple Silicon MacBook with a built-in trackpad
- Xcode Command Line Tools

The capture path loads Apple's private `MultitouchSupport.framework`. Its ABI can change between
macOS releases and it is not usable in the App Store.

Windows:

- Windows 11 x64
- Visual Studio with C++ desktop tools, Windows SDK/WDK 10.0.28000
- Administrator rights, test signing enabled for the driver

## Build

macOS agent:

```sh
make
./build/mac-touch-agent
```

Windows receiver:

```powershell
cmake -S windows -B out\windows -A x64
cmake --build out\windows --config Release
ctest --test-dir out\windows -C Release --output-on-failure
```

Windows driver:

```powershell
msbuild windows\driver\MtpVhfTouchpad.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /p:SignMode=Off
```

Sign the resulting `.sys` with your test certificate, run `Inf2Cat` to produce the catalog, sign the
catalog, then install:

```powershell
pnputil /add-driver windows\driver\x64\Release\MtpVhfTouchpad\MtpVhfTouchpad.inf /install /force
devcon install windows\driver\MtpVhfTouchpad.inf Root\MtpVhfTouchpad
```

Signing appends roughly 1.4 KB to the binary, so compare file timestamps or `DEVPKEY_Device_DriverVersion`
rather than file size when checking which build is loaded.

## Run

The receiver must run in the interactive desktop session; a service in session 0 receives no input
from the trackpad client and is not useful for debugging. Start it, then point the Mac at it:

```powershell
out\windows\Release\mtp-receiver.exe 39871
```

```sh
nc -vz WINDOWS_IP 39871
./build/mac-touch-agent WINDOWS_IP 39871
```

On disconnect or a 1.5 s active-contact timeout the receiver releases every contact.

The agent does not consume Mac input. To keep the built-in trackpad off the Mac while an external
mouse is attached, enable "Ignore built-in trackpad when mouse or wireless trackpad is present" in
Accessibility settings.

## Autostart

macOS keeps the agent running through a launch agent. Install the binary somewhere stable, then drop
a plist in `~/Library/LaunchAgents`. Replace `/Users/you` and the address with your own:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>local.touchpad-over-tcp</string>
    <key>ProgramArguments</key>
    <array>
        <string>/Users/you/bin/mac-touch-agent</string>
        <string>WINDOWS_IP</string>
        <string>39871</string>
    </array>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>ThrottleInterval</key><integer>10</integer>
    <key>LimitLoadToSessionType</key><string>Aqua</string>
    <key>StandardErrorPath</key><string>/Users/you/Library/Logs/touchpad-over-tcp/agent.log</string>
</dict>
</plist>
```

```sh
install -m 755 build/mac-touch-agent ~/bin/mac-touch-agent
mkdir -p ~/Library/Logs/touchpad-over-tcp
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/local.touchpad-over-tcp.plist
launchctl kickstart -k gui/$(id -u)/local.touchpad-over-tcp
```

The agent retries the connection once per second and stays quiet while Windows is unreachable, so it
can run permanently.

On Windows, start the receiver only while you are streaming. `deploy/windows/` has two wrappers that
hide the console; put `mtp-receiver.exe` next to them and call them from Sunshine's command
preparations (or a stream deck, or a shortcut):

```text
do:    deploy\windows\start-receiver.cmd
undo:  deploy\windows\stop-receiver.cmd
```

`mtp-receiver.exe --stop` connects to `127.0.0.1:39872` and asks the running instance to release every
contact and exit, so stopping never leaves a finger or a button latched in the driver. Killing the
process instead leaves the last report in place until the next start.

Both wrappers need the receiver to run as administrator, because the driver's device object is only
open to SYSTEM and Administrators. In Sunshine, enable "Run as Admin" for the preparation command.

## Docs

- `docs/WINDOWS_SETUP.zh-CN.md` — driver install walkthrough (Chinese)
- `docs/MAC_SETUP.md` — capture and agent setup
- `docs/VHF_STARTUP_ISSUE.md` — why the device can come up with Device Manager Code 10
- `docs/OPEN_SOURCE_REUSE.md` — which upstream implementations were studied and why

## Security notes

The link is plaintext TCP with no authentication. Keep port 39871 on a private network, do not
expose it, and treat any host on that LAN as able to move the pointer. The driver is test signed and
intended for a development machine only.

## Tools

`tools/` holds the analyzers used while bringing this up: raw capture analysis, trace inspection,
and synthetic MTP1 clients that exercise the receiver without a human on the trackpad. They are
plain scripts with no dependencies.

## License

Apache-2.0, see [LICENSE](LICENSE) and [NOTICE](NOTICE). Parts of the report descriptor follow the
MIT-licensed SPI implementation in `imbushuo/mac-precision-touchpad`; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

MacBook, macOS, Windows and Precision Touchpad are trademarks of their respective owners. This
project is not affiliated with or endorsed by Apple or Microsoft.
