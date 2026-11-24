"""Feeds a synthetic two-finger flick into the receiver over MTP1 so the glide
can be verified without a human on the trackpad.

Run it while the Mac agent is disconnected: the receiver serves one client.
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


def header(msg_type, payload, sequence, capture_us, count, flags):
    return (struct.pack('>IHHIIHHQQ', MAGIC, VERSION, msg_type, payload, sequence,
                        count, flags, capture_us, 0) + b'')


def contact(identifier, x, y, flags=TIP | IN_RANGE | CONFIDENCE):
    return struct.pack('>IBBHfffffffff', identifier, 4, flags, 0,
                       x, y, 0.0, 0.0, 0.2, 0.0, 3.0, 2.0, 0.0)


def message(msg_type, sequence, capture_us, contacts, flags=0):
    payload = len(contacts) * 44
    body = header(msg_type, payload, sequence, capture_us, len(contacts), flags)
    for c in contacts:
        body += contact(*c)
    return body


def main(host='127.0.0.1', port=39871):
    sock = socket.create_connection((host, port), timeout=3)
    seq = 1
    now = int(time.time() * 1_000_000)
    sock.sendall(message(HELLO, seq, now, []))
    seq += 1
    sock.sendall(message(RESET, seq, now, []))
    seq += 1

    # two fingers 4000 logical units apart, panning upward at 30000 units/s
    y = 0.60
    step = 30000 * 0.008 / 32767.0
    for index in range(30):
        now += 8000
        y -= step
        contacts = [(11, 0.40, y), (12, 0.52, y)]
        sock.sendall(message(FRAME, seq, now, contacts))
        seq += 1
        time.sleep(0.008)

    # lift: same contacts, tip cleared
    now += 8000
    contacts = [(11, 0.40, y, IN_RANGE | CONFIDENCE), (12, 0.52, y, IN_RANGE | CONFIDENCE)]
    sock.sendall(message(FRAME, seq, now, contacts))
    seq += 1
    now += 8000
    sock.sendall(message(FRAME, seq, now, []))
    # stay connected so the receiver can play the glide out
    time.sleep(1.6)
    sock.close()
    print('sent flick: %d frames, final y=%.3f' % (seq, y))


if __name__ == '__main__':
    host = sys.argv[1] if len(sys.argv) > 1 else '127.0.0.1'
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 39871
    main(host, port)
