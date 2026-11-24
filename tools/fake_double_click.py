"""Feeds a synthetic click or double click into the receiver over MTP1.

The finger stays on the panel for the whole sequence and only Button 1 toggles,
which is what a clickpad reports when the switch is pressed. Windows decides
"double click" from the interval between the two Button 1 downs and the distance
between the two pointer positions, so this is the exact sequence to replay when
double clicks do not register.

Run it while the Mac agent is disconnected: the receiver serves one client.

    python3 tools/fake_double_click.py 127.0.0.1 39871 --gap 80 --repeat 3
"""

import socket
import struct
import sys
import time

MAGIC = 0x4D545031
VERSION = 1
HELLO, FRAME, RESET = 1, 2, 3
TIP = 0x02
IN_RANGE = 0x01
CONFIDENCE = 0x04
BUTTON = 1 << 0


def header(msg_type, payload, sequence, capture_us, count, flags):
    return struct.pack('>IHHIIHHQQ', MAGIC, VERSION, msg_type, payload, sequence,
                       count, flags, capture_us, 0)


def contact(identifier, x, y, flags=TIP | IN_RANGE | CONFIDENCE):
    return struct.pack('>IBBHfffffffff', identifier, 4, flags, 0,
                       x, y, 0.0, 0.0, 0.2, 0.0, 3.0, 2.0, 0.0)


def message(msg_type, sequence, capture_us, contacts, flags=0):
    payload = len(contacts) * 44
    body = header(msg_type, payload, sequence, capture_us, len(contacts), flags)
    for entry in contacts:
        body += contact(*entry)
    return body


class Sender:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=3)
        self.sequence = 1
        self.now = int(time.time() * 1_000_000)

    def send(self, contacts, flags=0, step_us=8000):
        self.now += step_us
        self.sock.sendall(message(FRAME, self.sequence, self.now, contacts, flags))
        self.sequence += 1

    def hold(self, seconds):
        """Keep a finger on the panel so the pointer never lifts mid-sequence."""
        deadline = time.time() + seconds
        while time.time() < deadline:
            self.send([(1, 0.5, 0.5)])
            time.sleep(0.008)


def main():
    host = '127.0.0.1'
    port = 39871
    gap_ms = 80.0
    repeat = 3

    args = sys.argv[1:]
    index = 0
    positional = []
    while index < len(args):
        if args[index] == '--gap':
            gap_ms = float(args[index + 1])
            index += 2
        elif args[index] == '--repeat':
            repeat = int(args[index + 1])
            index += 2
        else:
            positional.append(args[index])
            index += 1
    if positional:
        host = positional[0]
    if len(positional) > 1:
        port = int(positional[1])

    sender = Sender(host, port)
    sender.sock.sendall(message(HELLO, sender.sequence, sender.now, []))
    sender.sequence += 1
    sender.sock.sendall(message(RESET, sender.sequence, sender.now, []))
    sender.sequence += 1

    sender.hold(0.15)

    for _ in range(repeat + 1):
        sender.send([(1, 0.5, 0.5)], BUTTON)
        sender.hold(0.04)
        sender.send([(1, 0.5, 0.5)])
        sender.hold(gap_ms / 1000.0)

    sender.hold(0.1)
    sender.send([(1, 0.5, 0.5, IN_RANGE | CONFIDENCE)])
    sender.send([], step_us=8000)
    time.sleep(0.4)
    sender.sock.close()
    print('sent %d presses, gap %.0f ms, %d frames'
          % (repeat + 1, gap_ms, sender.sequence - 1))


if __name__ == '__main__':
    main()
