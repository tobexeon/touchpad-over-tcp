#!/usr/bin/env python3
"""Replicate Windows TouchSession slot mapping against a Mac agent frame trace.

Detects whether the slot/id mapping the Windows receiver computes produces
position jumps that were NOT present in the raw Mac stream.
"""
import json
import math
import sys

MAX_CONTACTS = 5
LOGICAL_MAX = 32767
TIP = 0x02

path = sys.argv[1] if len(sys.argv) > 1 else "jitter-trace.jsonl"
raw = [json.loads(l) for l in open(path) if l.strip()]
frames = [f for f in raw if f.get("event") == "frame"]

slots = {}          # identifier -> slot
previous = {}       # slot -> last Contact (with x,y,flags)
last_frame_us = None
results = []        # (seq, [(slot,id,x,y,tip)])


def allocate_slot():
    used = set(slots.values())
    for c in range(MAX_CONTACTS):
        if c not in used:
            return c
    return None


def to_logical(v):
    return int(round(max(0.0, min(1.0, v)) * LOGICAL_MAX))


def to_logical_y(v):
    return to_logical(1.0 - v)


for f in frames:
    contacts = f["contacts"]
    present = set()
    next_contacts = {}

    for c in contacts:
        if not (c["flags"] & TIP):
            continue
        ident = c["id"]
        present.add(ident)
        if ident not in slots:
            slot = allocate_slot()
            if slot is None:
                continue
            slots[ident] = slot
        else:
            slot = slots[ident]
        next_contacts[slot] = c

    leaving = []
    for ident, slot in list(slots.items()):
        if ident not in present:
            leaving.append(ident)
            if slot in previous:
                rel = dict(previous[slot])
                rel["flags"] &= ~TIP
                next_contacts[slot] = rel

    emitted = []
    for slot in sorted(next_contacts):
        c = next_contacts[slot]
        emitted.append((slot, c["id"], to_logical(c["x"]), to_logical_y(c["y"]),
                        bool(c["flags"] & TIP)))

    for ident in leaving:
        slot = slots.pop(ident, None)
        if slot is not None:
            previous.pop(slot, None)
    for ident, slot in slots.items():
        found = next((c for c in contacts if c["id"] == ident), None)
        if found:
            previous[slot] = found

    results.append((f["sequence"], emitted))

# analyze pointer position (primary = lowest slot with TIP) between frames
jumps = []
for i in range(1, len(results)):
    seq, cur = results[i]
    _, prev = results[i - 1]
    prev_primary = next(((s, cid, x, y) for s, cid, x, y, tip in prev if tip), None)
    cur_primary = next(((s, cid, x, y) for s, cid, x, y, tip in cur if tip), None)
    if prev_primary and cur_primary:
        d = math.hypot(cur_primary[2] - prev_primary[2], cur_primary[3] - prev_primary[3])
        if d > 3000:  # > ~9% of logical range in one 8ms frame
            jumps.append((round(d), seq,
                          (prev_primary[0], prev_primary[1], prev_primary[2], prev_primary[3]),
                          (cur_primary[0], cur_primary[1], cur_primary[2], cur_primary[3])))

print(f"frames simulated: {len(results)}")
print(f"primary-pointer jumps >3000 logical units: {len(jumps)}")
for j in jumps[:25]:
    print("  ", j)

# also show slot changes for the dominant (single) contact
slot_changes = 0
for i in range(1, len(results)):
    cur = results[i][1]
    prev = results[i - 1][1]
    if len(cur) == 1 and len(prev) == 1 and cur[0][0] != prev[0][0]:
        slot_changes += 1
print(f"primary slot changes between consecutive single-touch frames: {slot_changes}")
