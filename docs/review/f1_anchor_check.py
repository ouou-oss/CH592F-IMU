#!/usr/bin/env python3
"""复现评审报告 §4 的 f1（样本编号 -> 节点 RTC）残差分析。

用法：
    python3 docs/review/f1_anchor_check.py [原始日志路径]

默认读取 data/gateway-live-20261118-02/raw/20261118/01_gw1.bin。
只依赖 Python 标准库，不读取 Parquet/CSV，也不依赖已被删除的 Python PC 工具。

原始日志记录格式（docs/03 §2.2）：u64 pc_recv_ns + u16 len + 主机帧原文。
本脚本自己拆主机帧、验 CRC、解 RF_RX、解 DATA 包头，因此可以独立验证
docs/11 记录的 f1 = 615.207 µs 是怎样得到的，以及它与拟合窗口长度的关系。
"""

import struct
import sys
import statistics as st

RTC_MODULUS = 0xA8C00000
SAMPLES_PER_PACKET = 35
DEFAULT_RAW = "data/gateway-live-20261118-02/raw/20261118/01_gw1.bin"


def crc16_ccitt_false(data):
    """CRC-16/CCITT-FALSE：poly 0x1021, init 0xFFFF, 不反转, 无最终异或。"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (0x1021 if crc & 0x8000 else 0)) & 0xFFFF
    return crc


def read_records(path):
    """按 docs/03 §2.2 切出原始日志记录，返回主机帧列表。"""
    blob = open(path, "rb").read()
    frames = []
    pos = 0
    while pos + 10 <= len(blob):
        pc_ns, length = struct.unpack_from("<QH", blob, pos)
        pos += 10
        if pos + length > len(blob):
            break
        frames.append((pc_ns, blob[pos:pos + length]))
        pos += length
    return frames


def host_frames(frames):
    """只保留 CRC 正确的主机帧，返回 (type, frame_seq, payload)。"""
    out = []
    for _pc_ns, frame in frames:
        if len(frame) < 8 or frame[0] != 0xA5 or frame[1] != 0x5A:
            continue
        length = struct.unpack_from("<H", frame, 4)[0]
        if len(frame) != length + 8:
            continue
        if crc16_ccitt_false(frame[2:6 + length]) != struct.unpack_from("<H", frame, 6 + length)[0]:
            continue
        out.append((frame[2], frame[3], frame[6:6 + length]))
    return out


def data_packets(payloads, boot):
    """按 (boot_id, seq) 去重，返回指定 boot 的唯一 DATA 包，按 seq 升序。"""
    seen = {}
    for payload in payloads:
        if len(payload) < 7:
            continue
        rf_len = payload[6]
        rf = payload[7:7 + rf_len]
        if len(rf) < 36 or (rf[0] >> 4) != 1 or (rf[0] & 15) != 1:
            continue
        if struct.unpack_from("<H", rf, 4)[0] != boot:
            continue
        seq = struct.unpack_from("<I", rf, 6)[0]
        seen.setdefault(seq, {
            "seq": seq,
            "first": struct.unpack_from("<I", rf, 14)[0],
            "n": rf[18],
            "attempt": rf[20],
            "segment": rf[19],
            "anchor_idx": struct.unpack_from("<I", rf, 22)[0],
            "anchor_rtc": struct.unpack_from("<I", rf, 26)[0],
        })
    return [seen[key] for key in sorted(seen)]


def unwrap(values):
    """把 u32 的 RTC 计数展开成单调序列，模数为 RTC_MAX_COUNT。"""
    out = []
    previous = None
    for value in values:
        if previous is not None and value < previous and previous - value > RTC_MODULUS // 2:
            value += RTC_MODULUS
        out.append(value)
        previous = value
    return out


def fit(xs, ys):
    """最小二乘直线拟合，返回 (slope, intercept, residuals)。"""
    count = len(xs)
    mean_x = sum(xs) / count
    mean_y = sum(ys) / count
    denominator = sum((x - mean_x) ** 2 for x in xs)
    slope = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys)) / denominator
    intercept = mean_y - slope * mean_x
    return slope, intercept, [y - (slope * x + intercept) for x, y in zip(xs, ys)]


def windowed_rms(xs, ys, window):
    """按固定包数切窗分别拟合，拼接残差后返回 RMS（单位：RTC tick）。"""
    residuals = []
    start = 0
    while start < len(xs):
        end = min(start + window, len(xs))
        if end - start >= 4:  # 少于 4 点的直线拟合没有残差自由度
            residuals += fit(xs[start:end], ys[start:end])[2]
        start = end
    return st.pstdev(residuals) if len(residuals) > 1 else 0.0


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_RAW
    payloads = [payload for _type, _seq, payload in host_frames(read_records(path))]
    packets = data_packets(payloads, 6)
    if not packets:
        raise SystemExit("没有解析到 boot 6 的 DATA 包，请确认路径与 boot_id")

    anchors = unwrap([p["anchor_rtc"] for p in packets])
    xs = [float(p["anchor_idx"]) for p in packets]
    ys = [float(value) for value in anchors]

    seams = sum(1 for a, b in zip(packets, packets[1:]) if b["first"] != a["first"] + a["n"])
    seq_gaps = sum(1 for a, b in zip(packets, packets[1:]) if b["seq"] != a["seq"] + 1)
    print("原始日志          : %s" % path)
    print("boot 6 唯一 DATA 包: %d" % len(packets))
    print("样本数范围        : n=%d..%d" % (min(p["n"] for p in packets), max(p["n"] for p in packets)))
    print("segment 集合       : %s" % sorted({p["segment"] for p in packets}))
    print("样本编号不连续次数 : %d" % seams)
    print("seq 缺口次数       : %d" % seq_gaps)
    print()

    slope, _intercept, residuals = fit(xs, ys)
    print("整段单窗口拟合：")
    print("  斜率             : %.6f RTC tick/样本" % slope)
    print("  等效采样率(RTC)  : %.2f Hz" % (32768.0 / slope))
    print("  残差 RMS         : %.2f tick = %.1f us"
          % (st.pstdev(residuals), st.pstdev(residuals) * 1e6 / 32768.0))
    print()

    steps = [ys[i + 1] - ys[i] for i in range(len(ys) - 1)]
    print("相邻包锚点差（应等于 35 样本的实际间隔）：")
    print("  均值 %.2f tick, 标准差 %.2f tick, 取值个数 %d"
          % (st.mean(steps), st.pstdev(steps), len(set(steps))))
    print()

    print("残差 RMS 随拟合窗口长度变化：")
    for window in (35, 70, 175, 350, 700, 1400):
        rms = windowed_rms(xs, ys, window)
        print("  %5d 包 ≈ %6.2f s : %6.2f tick = %7.1f us"
              % (window, window * SAMPLES_PER_PACKET / 3200.0, rms, rms * 1e6 / 32768.0))
    print("  %5d 包 ≈ %6.2f s : %6.2f tick = %7.1f us  (整段)"
          % (len(xs), len(xs) * SAMPLES_PER_PACKET / 3200.0,
             st.pstdev(residuals), st.pstdev(residuals) * 1e6 / 32768.0))
    print()
    window = 175
    if len(xs) >= window:
        blocks = [st.pstdev(fit(xs[i:i + window], ys[i:i + window])[2]) * 1e6 / 32768.0
                  for i in range(0, len(xs) - window + 1, window)]
        sliding = [st.pstdev(fit(xs[i:i + window], ys[i:i + window])[2]) * 1e6 / 32768.0
                   for i in range(len(xs) - window + 1)]
        print("175 包完整分块最大 RMS：%.2f us" % max(blocks))
        print("175 包滑动窗口最大 RMS：%.2f us；超过 50 us：%d/%d"
              % (max(sliding), sum(value > 50 for value in sliding), len(sliding)))
    print("合并残差 RMS 不是每个窗口的上界；缩短窗口不等于所有窗口达标。")
    print("恒定频偏会被斜率吸收；残差与时长之比不能直接确定时钟漂移量，")
    print("也不能由锚点的局部残差推出样本的绝对时间精度。")


if __name__ == "__main__":
    main()
