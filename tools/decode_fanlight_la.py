#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把逻辑分析仪抓到的风扇灯遥控器 OOK 基带，还原成 ESPHome 的 raw 码表。

用法：
  python tools/decode_fanlight_la.py "C:/.../灯开关.sr"
  python tools/decode_fanlight_la.py capture.csv --rate 4000000
  python tools/decode_fanlight_la.py capture.sr --rate 24000000 --channel 3

输入支持：
  - sigrok / PulseView 的 .sr（srzip：zip 内 logic-*-N 分块，每字节 8 通道，bit0=D0）
  - 两列 CSV：time_s,level（0/1，按跳变或逐采样均可）

输出：
  1) 自动找出哪一路是 OOK 数据线
  2) 每帧的 32 位码、帧间静默、一次按键发了多少帧、松手后的行为
  3) 校验和检查（第 4 字节是否为前 3 字节之和的低 8 位）
  4) 可直接粘进 fanlight_tx.h 的 66 项 raw 数组与 repeat / wait_time 建议
"""

import argparse
import csv
import re
import sys
import zipfile

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

PULSE = 250  # 标称基本脉冲 us
SYNC_HIGH = 250
SYNC_LOW = 7750
BIT_HIGH_1, BIT_LOW_1 = 750, 250
BIT_HIGH_0, BIT_LOW_0 = 250, 750


def parse_rate(text):
    m = re.search(r"([\d.]+)\s*(MHz|kHz|Hz)?", str(text), re.I)
    if not m:
        return None
    v = float(m.group(1))
    unit = (m.group(2) or "Hz").lower()
    return int(v * {"mhz": 1e6, "khz": 1e3, "hz": 1}[unit])


def load_sr(path):
    z = zipfile.ZipFile(path)
    rate = None
    if "metadata" in z.namelist():
        meta = z.read("metadata").decode("utf-8", "replace")
        m = re.search(r"samplerate\s*=\s*(.+)", meta)
        if m:
            rate = parse_rate(m.group(1))
    parts = []
    for name in z.namelist():
        m = re.match(r"logic-\d+-(\d+)$", name)
        if m:
            parts.append((int(m.group(1)), name))
    parts.sort()
    if not parts:
        raise SystemExit("这个 .sr 里没有 logic-*-N 采样块，请导出成 CSV 再试")
    data = b"".join(z.read(n) for _, n in parts)
    return data, rate or 24000000, 8


def load_csv(path, rate):
    times, levels = [], []
    with open(path, newline="", encoding="utf-8-sig") as f:
        for row in csv.reader(f):
            if len(row) < 2 or row[0].startswith("#"):
                continue
            try:
                t = float(row[0])
                v = int(float(row[1]))
            except ValueError:
                continue
            times.append(t)
            levels.append(v & 1)
    if not times:
        raise SystemExit("CSV 里没读到 time,level 数据")
    return times, levels, rate


def channel_edges(data, ch, rate):
    """返回 (初始电平, [(跳变样本下标, 跳变后电平), ...])，纯标准库。"""
    table = bytes(((i >> ch) & 1) for i in range(256))
    bits = data.translate(table)
    edges = []
    pos = 0
    find_up, find_down = bits.find, bits.find
    up_pat, down_pat = b"\x00\x01", b"\x01\x00"
    while True:
        up = find_up(up_pat, pos)
        down = find_down(down_pat, pos)
        if up < 0 and down < 0:
            break
        k = min(x for x in (up, down) if x >= 0)
        edges.append((k + 1, bits[k + 1]))
        pos = k + 1
    return bits[0] if bits else 0, edges


def to_segments(initial, edges, rate, is_csv=False, times=None, levels=None):
    """段列表 [(电平, 起始秒, 时长秒), ...]"""
    segs = []
    if is_csv:
        pts = []
        for t, l in zip(times, levels):
            if pts and pts[-1][1] == l:
                continue
            pts.append((t, l))
        return [(l, t, (pts[j + 1][0] if j + 1 < len(pts) else t) - t)
                for j, (t, l) in enumerate(pts)]
    prev_l, prev_i = initial, 0
    for i, l in edges:
        segs.append((prev_l, prev_i / rate, (i - prev_i) / rate))
        prev_l, prev_i = l, i
    segs.append((prev_l, prev_i / rate, 0.0))
    return segs


def near(v, target, tol=0.25):
    return abs(v - target) <= target * tol


def decode_frames(segs):
    """找出同步头 + 32 位帧，返回帧列表。"""
    us = [(l, t * 1e6, d * 1e6) for l, t, d in segs]
    frames = []
    i = 0
    n = len(us)
    while i + 66 <= n:
        l0, t0, d0 = us[i]
        l1, _, d1 = us[i + 1]
        if not (l0 == 1 and near(d0, SYNC_HIGH) and l1 == 0 and near(d1, SYNC_LOW)):
            i += 1
            continue
        bits = ""
        raw = [int(round(d0)), -int(round(d1))]
        ok = True
        for k in range(32):
            hi_l, _, hi = us[i + 2 + 2 * k]
            lo_l, _, lo = us[i + 3 + 2 * k]
            if hi_l != 1 or lo_l != 0:
                ok = False
                break
            one = near(hi, BIT_HIGH_1, 0.35)
            zero = near(hi, BIT_HIGH_0, 0.35)
            if one == zero:
                ok = False
                break
            bits += "1" if one else "0"
            raw += [BIT_HIGH_1 if one else BIT_HIGH_0, -(BIT_LOW_1 if one else BIT_LOW_0)]
        if not ok:
            i += 1
            continue
        # 最后一位的低电平段通常被拉长（帧尾 250/750us + 帧间静默，
        # 直到下一个上升沿）。帧的「真正结束」按标称位低电平算，
        # 多出来的低电平就是帧间静默 / 松手后的静默。
        _, last_start, _ = us[i + 65]
        tail_bit = BIT_LOW_1 if bits[-1] == "1" else BIT_LOW_0
        end = (last_start + tail_bit) / 1e6
        frames.append({
            "t": t0 / 1e6,
            "end": end,
            "value": int(bits, 2),
            "bits": bits,
            "raw": raw,
            "sync": (d0, d1),
        })
        i += 66
    return frames


def group_bursts(frames, gap_ms=100):
    groups, cur = [], []
    for f in frames:
        if cur and (f["t"] - cur[-1]["end"]) * 1000 > gap_ms:
            groups.append(cur)
            cur = []
        cur.append(f)
    if cur:
        groups.append(cur)
    return groups


def median(xs):
    xs = sorted(xs)
    return xs[len(xs) // 2] if xs else 0


def main():
    ap = argparse.ArgumentParser(description="解码风扇灯遥控器逻辑分析仪抓包")
    ap.add_argument("path", help=".sr 或两列 CSV")
    ap.add_argument("--rate", type=int, default=0, help="采样率 Hz（.sr 会自动读 metadata）")
    ap.add_argument("--channel", type=int, default=-1, help="只分析指定通道，默认自动扫描全部")
    ap.add_argument("--burst-gap", type=float, default=100, help="帧间隔超过多少 ms 算另一次按键，默认 100")
    args = ap.parse_args()

    is_csv = args.path.lower().endswith(".csv")
    if is_csv:
        if not args.rate:
            raise SystemExit("CSV 需要 --rate，例如 --rate 4000000")
        times, levels, rate = load_csv(args.path, args.rate)
        channels = [0]
        raw_data = None
    else:
        raw_data, rate, channels = load_sr(args.path)
        if args.rate:
            rate = args.rate
        channels = [args.channel] if args.channel >= 0 else list(range(channels))

    print("采样率 = %d Hz（%.2f MHz）" % (rate, rate / 1e6))

    best = None
    for ch in channels:
        if is_csv:
            segs = to_segments(0, [], rate, True, times, levels)
        else:
            initial, edges = channel_edges(raw_data, ch, rate)
            if len(edges) < 100:
                print("D%d: 无有效跳变" % ch)
                continue
            segs = to_segments(initial, edges, rate)
        frames = decode_frames(segs)
        print("D%d: 边沿 %d 个，解出 OOK 帧 %d 个" % (ch, len(segs) - 1, len(frames)))
        if frames and (best is None or len(frames) > len(best[1])):
            best = (ch, frames)

    if not best:
        print("\n没解出任何 OOK 帧。可能这一路不是数据线，或采样率不对。"
              "\n确认：同步头应为 250us 高 + 7750us 低，位宽 250/750us。")
        return

    ch, frames = best
    print("\n=== 数据线 D%d，共 %d 帧 ===" % (ch, len(frames)))
    groups = group_bursts(frames, args.burst_gap)
    for gi, g in enumerate(groups, 1):
        gaps = [(g[j + 1]["t"] - g[j]["end"]) * 1000 for j in range(len(g) - 1)]
        print("按键段 %d：%d 帧，时长 %.1f ms，帧间静默 %s ms，码 %s" % (
            gi, len(g), (g[-1]["end"] - g[0]["t"]) * 1000,
            ", ".join("%.2f" % x for x in gaps[:12]) or "-",
            " ".join("%08X" % f["value"] for f in g[:12])))

    print("\n=== 码值与校验和 ===")
    seen = {}
    for f in frames:
        v = f["value"]
        if v in seen:
            continue
        seen[v] = f
        chk = ((v >> 24) + ((v >> 16) & 0xFF) + ((v >> 8) & 0xFF)) & 0xFF
        print("0x%08X  第4字节 0x%02X  前三字节和低8位 0x%02X  %s  "
              "同步 %.0f/%.0f us" % (
                  v, v & 0xFF, chk, "校验一致" if (v & 0xFF) == chk else "校验不一致",
                  f["sync"][0], f["sync"][1]))

    all_gaps = [(frames[i + 1]["t"] - frames[i]["end"]) * 1000
                for i in range(len(frames) - 1)
                if (frames[i + 1]["t"] - frames[i]["end"]) * 1000 < args.burst_gap]
    print("\n=== 发射参数建议 ===")
    print("repeat（每次按键的帧数）≈ %d" % median([len(g) for g in groups]))
    print("帧间静默中位数 = %.2f ms（含帧尾 0.75 ms，wait_time ≈ %.2f ms）" % (
        median(all_gaps), max(0, median(all_gaps) - 0.75)))

    print("\n=== 可粘贴的 raw 码表（按出现顺序去重）===")
    for v, f in seen.items():
        print("  // 0x%08X" % v)
        items = f["raw"]
        for i in range(0, len(items), 11):
            print("    " + ", ".join(str(x) for x in items[i:i + 11]) + ",")


if __name__ == "__main__":
    main()
