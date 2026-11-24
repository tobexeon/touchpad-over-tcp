#!/usr/bin/env python3
"""分析 mac-touch-agent --button-trace 输出，抓取指针跳变的可疑特征。

维度:
  1) 帧间隔超时 (>200ms 会让 Windows Receiver ReleaseAll -> 指针跳)
  2) 坐标突变 (归一化坐标 0..1, 相邻帧同 id 位移过大)
  3) 触点 id 突变 (单指段内 id 变化 / id handoff 位置不连续)
  4) 按钮状态抖动 (frame_flags 快速翻转, 快速双击场景)
"""
import json
import math
import sys
from collections import Counter

path = sys.argv[1] if len(sys.argv) > 1 else "jitter-trace.jsonl"
rows = []
for line in open(path):
    line = line.strip()
    if not line:
        continue
    try:
        o = json.loads(line)
    except Exception:
        continue
    if o.get("event") == "frame":
        rows.append(o)

print(f"frame rows: {len(rows)}")
if not rows:
    sys.exit(0)

seqs = [r["sequence"] for r in rows]
print(f"sequence: {seqs[0]} .. {seqs[-1]} (total {seqs[-1]-seqs[0]+1})")

# ---- 1) 帧间隔 ----
TIMEOUT_US = 200_000
gaps = []
for i in range(1, len(rows)):
    dt = rows[i]["time_us"] - rows[i - 1]["time_us"]
    if dt > TIMEOUT_US:
        gaps.append((round(dt / 1000, 1), rows[i - 1]["sequence"], rows[i]["sequence"],
                     rows[i - 1]["contact_count"], rows[i]["contact_count"]))
gaps.sort(reverse=True)
print(f"\n[1] frame gaps > {TIMEOUT_US//1000}ms: {len(gaps)}")
for g in gaps[:20]:
    print(f"   gap={g[0]}ms seq {g[1]}->{g[2]} contacts {g[3]}->{g[4]}")

# ---- 2) 坐标突变 (同 id, 两帧都在 board state 3/4) ----
jumps = []
for i in range(1, len(rows)):
    prev = {c["id"]: c for c in rows[i - 1]["contacts"]}
    for c in rows[i]["contacts"]:
        cid = c["id"]
        if cid in prev and c["state"] in (3, 4) and prev[cid]["state"] in (3, 4):
            d = math.hypot(c["x"] - prev[cid]["x"], c["y"] - prev[cid]["y"])
            jumps.append((round(d, 4), rows[i]["sequence"], cid,
                          (round(prev[cid]["x"], 4), round(prev[cid]["y"], 4)),
                          (round(c["x"], 4), round(c["y"], 4))))
jumps.sort(reverse=True)
print(f"\n[2] per-id coord step: max={jumps[0][0] if jumps else 0} "
      f"(normalized 0..1)")
print("   top 20 steps:")
for j in jumps[:20]:
    print(f"   d={j[0]} seq={j[1]} id={j[2]} {j[3]}->{j[4]}")
# 分布
for th in (0.05, 0.1, 0.2, 0.5):
    print(f"   steps > {th}: {sum(1 for j in jumps if j[0] > th)}")

# ---- 3) 单指段内 id 变化 ----
runs = []
cur = []
for r in rows:
    if r["contact_count"] == 1 and r["contacts"] and r["contacts"][0]["state"] in (3, 4):
        cur.append(r)
    else:
        if len(cur) >= 3:
            runs.append(cur)
        cur = []
if len(cur) >= 3:
    runs.append(cur)
id_changes = 0
suspect_runs = []
for r in runs:
    ids = [x["contacts"][0]["id"] for x in r]
    transitions = [(r[i]["sequence"], ids[i - 1], ids[i])
                   for i in range(1, len(ids)) if ids[i] != ids[i - 1]]
    if transitions:
        id_changes += len(transitions)
        suspect_runs.append((r[0]["sequence"], r[-1]["sequence"], len(r),
                             sorted(set(ids)), transitions[:5]))
print(f"\n[3] single-finger board-contact runs: {len(runs)}, id-changes inside: {id_changes}")
for s in suspect_runs[:15]:
    print(f"   run seq {s[0]}-{s[1]} len={s[2]} ids={s[3]} changes={s[4]}")

# id handoff 位置连续性
handoffs = []
for i in range(1, len(rows)):
    prevm = {c["id"]: c for c in rows[i - 1]["contacts"]}
    nowm = {c["id"]: c for c in rows[i]["contacts"]}
    gone = set(prevm) - set(nowm)
    appeared = set(nowm) - set(prevm)
    if len(gone) == 1 and len(appeared) == 1:
        old = prevm[list(gone)[0]]
        new = nowm[list(appeared)[0]]
        d = math.hypot(new["x"] - old["x"], new["y"] - old["y"])
        handoffs.append((round(d, 4), rows[i]["sequence"], old["id"], new["id"],
                         old["state"], new["state"]))
handoffs.sort(reverse=True)
print(f"   id handoffs total: {len(handoffs)}, top 10 by distance:")
for h in handoffs[:10]:
    print(f"   d={h[0]} seq={h[1]} id {h[2]}->{h[3]} state {h[4]}->{h[5]}")

# ---- 4) 按钮状态抖动 ----
flags_seq = [(r["sequence"], r["frame_flags"]) for r in rows]
flips = []
for i in range(1, len(flags_seq)):
    if flags_seq[i][1] != flags_seq[i - 1][1]:
        flips.append((flags_seq[i - 1][0], flags_seq[i][0],
                      flags_seq[i - 1][1], flags_seq[i][1]))
print(f"\n[4] frame_flags transitions: {len(flips)}")
fc = Counter((a[2], a[3]) for a in flips)
for k, v in fc.most_common():
    print(f"   {k[0]}->{k[1]}: {v}")
# 短脉冲: 按下后 <=3 帧内抬起
short = []
for i in range(1, len(flips)):
    if flips[i][2] == 1 and flips[i][3] == 0 and flips[i - 1][3] == 1:
        span = flips[i][0] - flips[i - 1][0]
        if span <= 5:
            short.append((flips[i - 1][0], flips[i][0], span))
print(f"   short button pulses (down->up within <=5 frames): {len(short)}")
for s in short[:15]:
    print(f"   seq {s[0]}->{s[1]} span={s[2]}")
