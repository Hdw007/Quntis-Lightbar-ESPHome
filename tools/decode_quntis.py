#!/usr/bin/env python3
"""
Demodulator, XN297L packet decoder, and IQ packet synthesizer for the
Quntis Monitor Lightbar Remote (.complex16s at 10 MSps, 2.419 GHz).
"""
from collections import OrderedDict
import glob
import os
import sys
import numpy as np

XN297_PREAMBLE_BITS = "011100010000111101010101"  # 0x71, 0x0F, 0x55

XN297_SCRAMBLE = [
    0xE3, 0xB1, 0x4B, 0xEA, 0x85, 0xBC, 0xE5, 0x66,
    0x0D, 0xAE, 0x8C, 0x88, 0x12, 0x69, 0xEE, 0x1F,
    0xC7, 0x62, 0x97, 0xD5, 0x0B, 0x79, 0xCA, 0xCC,
    0x1B, 0x5D, 0x19, 0x10, 0x24, 0xD3, 0xDC, 0x3F,
    0x8E, 0xC5, 0x2F, 0xAA, 0x16, 0xF3, 0x95, 0x98,
]

XN297_CRC_XOROUT_SCRAMBLED = [
    0x0000, 0x3448, 0x9BA7, 0x8BBB, 0x85E1, 0x3E8C, 0x451E, 0x18E6,
    0x6B24, 0xE7AB, 0x3828, 0x814B, 0xD461, 0xF494, 0x2503, 0x691D,
    0xFE8B, 0x3AF1, 0x25BC, 0x6152, 0xC09C, 0x14EE, 0x4879, 0x7E3F,
    0x4642, 0x4C7E, 0x1171, 0xC499,
]

CMD_NAMES = {
    0x31: "Power Toggle (On/Off)",
    0x32: "RGB Backlight On / Cycle Mode",
    0x35: "Brightness Up",
    0x36: "Brightness Down",
    0x39: "Color Temperature",
    0x3D: "RGB Backlight Off",
}


def bit_reverse(byte_val: int) -> int:
    return int(f"{byte_val:08b}"[::-1], 2)


def crc16_update(crc: int, byte_val: int, bits: int = 8) -> int:
    crc ^= byte_val << 8
    for _ in range(bits):
        if crc & 0x8000:
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF
        else:
            crc = (crc << 1) & 0xFFFF
    return crc


def demodulate_burst_iq(raw_seg: np.ndarray, fs: float = 10e6, sps: int = 10) -> str:
    i_ch = raw_seg[0::2].astype(np.float32)
    q_ch = raw_seg[1::2].astype(np.float32)

    # Detect and unwrap int8 negative saturation wraparound (e.g., -130 wrapping to +126 on Q)
    if np.mean(q_ch) > 30.0 and np.sum(q_ch == 126) > 100:
        q_ch = np.where(q_ch >= 125, -127.0, q_ch)
    if np.mean(i_ch) > 30.0 and np.sum(i_ch == 126) > 100:
        i_ch = np.where(i_ch >= 125, -127.0, i_ch)

    iq = i_ch + 1j * q_ch
    phase_diff = np.angle(iq[1:] * np.conj(iq[:-1]))
    inst_freq = phase_diff * fs / (2 * np.pi)
    smooth_freq = np.convolve(inst_freq, np.ones(5) / 5, mode="same")

    mag = np.abs(iq[:-1])
    active = np.where(mag > mag.max() * 0.4)[0]
    if len(active) < 500:
        return ""
    start_idx, end_idx = active[0], active[-1]

    mod_region = smooth_freq[start_idx + (end_idx - start_idx) // 2 : end_idx]
    f_low = np.percentile(mod_region, 15)
    f_high = np.percentile(mod_region, 85)
    f_center = 0.5 * (f_low + f_high)

    above = smooth_freq[start_idx:end_idx] > f_center
    crossings = np.where(np.diff(above.astype(int)) != 0)[0] + start_idx
    if len(crossings) < 20:
        return ""
    mod_crossings = crossings[-40:]
    transition_phase = int(np.round(np.median(mod_crossings % sps)))
    center_phase = (transition_phase + sps // 2) % sps

    first_sample = start_idx + ((center_phase - start_idx) % sps)
    sample_centers = np.arange(first_sample, end_idx, sps)
    return "".join("1" if smooth_freq[c] > f_center else "0" for c in sample_centers)


def demodulate_complex16s(filepath: str, fs: float = 10e6, sps: int = 10) -> str:
    raw = np.fromfile(filepath, dtype=np.int8)
    return demodulate_burst_iq(raw, fs=fs, sps=sps)


def parse_xn297_bits(bits: str, addr_len: int = 5):
    idx = bits.find(XN297_PREAMBLE_BITS)
    if idx == -1:
        return None

    after_preamble = bits[idx + len(XN297_PREAMBLE_BITS) :]
    raw_bytes = [
        int(after_preamble[i : i + 8], 2)
        for i in range(0, len(after_preamble) - 7, 8)
    ]

    for body_len in range(3, min(len(raw_bytes) - 1, len(XN297_CRC_XOROUT_SCRAMBLED) + 3)):
        crc = 0xB5D2
        for b in raw_bytes[:body_len]:
            crc = crc16_update(crc, b)
        expected_crc = crc ^ XN297_CRC_XOROUT_SCRAMBLED[body_len - 3]
        rx_crc = (raw_bytes[body_len] << 8) | raw_bytes[body_len + 1]
        if expected_crc == rx_crc:
            body = raw_bytes[:body_len]
            addr_wire = [body[i] ^ XN297_SCRAMBLE[i] for i in range(addr_len)]
            addr_pipe = addr_wire[::-1]
            p = [bit_reverse(body[i] ^ XN297_SCRAMBLE[i]) for i in range(addr_len, body_len)]
            p_msb = [body[i] ^ XN297_SCRAMBLE[i] for i in range(addr_len, body_len)]
            return {
                "preamble_idx": idx,
                "body": body,
                "rx_crc": rx_crc,
                "addr_wire": addr_wire,
                "addr_pipe": addr_pipe,
                "p": p,
                "p_msb": p_msb,
            }
    return None


def decode_xn297_bits(bits: str, addr_len: int = 5) -> None:
    res = parse_xn297_bits(bits, addr_len=addr_len)
    if res is None:
        print("Could not find valid XN297 packet in bitstream.")
        return

    p = res["p"]
    checksum_calc = sum(p[:-1]) & 0xFF
    checksum_ok = checksum_calc == p[-1]
    cmd_desc = CMD_NAMES.get(p[4], "Unknown")

    print(f"Preamble offset : bit {res['preamble_idx']} (0x71 0x0F 0x55)")
    print(f"Scrambled bytes : {' '.join(f'{b:02X}' for b in res['body'])}")
    print(f"CRC-16 (on-air) : 0x{res['rx_crc']:04X} (VALID XN297 CRC)")
    print(f"Address (pipe)  : {' '.join(f'{b:02X}' for b in res['addr_pipe'])}  (wire: {' '.join(f'{b:02X}' for b in res['addr_wire'])})")
    print(f"Payload (LSB)   : {' '.join(f'{b:02X}' for b in p)}")
    if len(p) == 11:
        rgb_on = bool(p[8] & 0x80)
        rgb_mode = p[8] & 0x7F
        print(
            f"  Header     p[0..3]: {' '.join(f'{b:02X}' for b in p[0:4])}\n"
            f"  Command    p[4]   : 0x{p[4]:02X} ({cmd_desc})\n"
            f"  Brightness p[5]   : 0x{p[5]:02X} ({p[5]:3d} / 255 = {p[5]/255*100:5.1f}%)\n"
            f"  ColorTemp  p[6]   : 0x{p[6]:02X} ({p[6]:3d} / 255 = {p[6]/255*100:5.1f}%)\n"
            f"  Power      p[7]   : 0x{p[7]:02X} ({'ON' if p[7] == 0xFF else 'OFF' if p[7] == 0x00 else p[7]})\n"
            f"  RGB State  p[8]   : 0x{p[8]:02X} (Backlight={'ON' if rgb_on else 'OFF'}, Mode={rgb_mode}/15)\n"
            f"  SeqCounter p[9]   : 0x{p[9]:02X} ({p[9]:3d})\n"
            f"  Checksum   p[10]  : 0x{p[10]:02X} (sum(p[0..9])&0xFF = 0x{checksum_calc:02X} -> {'VALID' if checksum_ok else 'MISMATCH'})"
        )


def decode_multiburst_file(filepath: str, fs: float = 10e6, sps: int = 10) -> None:
    raw = np.fromfile(filepath, dtype=np.int8)
    n_blocks = len(raw) // 200
    blocks = raw[: n_blocks * 200].reshape(n_blocks, 200).astype(np.float32)
    power = np.mean(np.abs(blocks), axis=1)

    active_blocks = np.where(power > 9.5)[0]
    bursts = []
    if len(active_blocks) > 0:
        start = active_blocks[0]
        prev = active_blocks[0]
        for b in active_blocks[1:]:
            if b - prev > 5:
                if (prev + 1 - start) >= 35:
                    bursts.append((start * 100, (prev + 1) * 100))
                start = b
            prev = b
        if (prev + 1 - start) >= 35:
            bursts.append((start * 100, (prev + 1) * 100))

    groups = OrderedDict()
    for s, e in bursts:
        s_pad = max(0, s - 100)
        e_pad = min(len(raw) // 2, e + 100)
        bits = demodulate_burst_iq(raw[s_pad * 2 : e_pad * 2], fs=fs, sps=sps)
        res = parse_xn297_bits(bits)
        if res is not None:
            key = tuple(res["p"])
            t_sec = s / fs
            if key not in groups:
                groups[key] = {"count": 0, "first": t_sec, "last": t_sec, "bits": bits}
            groups[key]["count"] += 1
            groups[key]["last"] = t_sec

    print(f"Found {len(bursts)} RF bursts across {len(groups)} distinct button press(es):\n")
    for idx, (key, info) in enumerate(groups.items(), 1):
        dur_ms = (info["last"] - info["first"]) * 1e3
        print(f"--- Button Press #{idx} ({info['count']} valid bursts, {info['first']:.3f}s .. {info['last']:.3f}s, span={dur_ms:.1f} ms) ---")
        decode_xn297_bits(info["bits"])
        print()


def build_xn297_packet_bits(addr_pipe: list[int], payload_10_bytes: list[int]) -> str:
    """
    Given a 5-byte pipe address and the first 10 bytes of the payload p[0..9],
    computes p[10] = sum(p[0..9]) & 0xFF, scrambles the address + payload,
    appends the XN297 CRC-16, and returns the on-air bitstring.
    """
    assert len(addr_pipe) == 5 and len(payload_10_bytes) == 10
    chk = sum(payload_10_bytes) & 0xFF
    full_payload = list(payload_10_bytes) + [chk]

    addr_wire = addr_pipe[::-1]
    raw_unscrambled = addr_wire + [bit_reverse(b) for b in full_payload]
    scrambled_body = [raw_unscrambled[i] ^ XN297_SCRAMBLE[i] for i in range(16)]

    crc = 0xB5D2
    for b in scrambled_body:
        crc = crc16_update(crc, b)
    crc ^= XN297_CRC_XOROUT_SCRAMBLED[16 - 3]
    crc_bytes = [(crc >> 8) & 0xFF, crc & 0xFF]

    on_air_bytes = [0x71, 0x0F, 0x55] + scrambled_body + crc_bytes
    frame_bits = "1100" + "".join(f"{b:08b}" for b in on_air_bytes) + "1010111111"
    return frame_bits


def synthesize_complex16s(
    filepath: str,
    addr_pipe: list[int],
    payload_10_bytes: list[int],
    fs: float = 10e6,
    sps: int = 10,
    f_center: float = 284e3,
    f_dev: float = 291e3,
) -> None:
    """
    Synthesizes a HackRF-compatible .complex16s file (int8 IQ) with the exact
    235 us carrier warmup and 1 Mbps GFSK modulation of the original remote.
    """
    mod_bits = build_xn297_packet_bits(addr_pipe, payload_10_bytes)
    freq_profile = np.concatenate([
        np.zeros(70),
        np.full(2350, f_center),
        np.repeat([f_center + f_dev if b == "1" else f_center - f_dev for b in mod_bits], sps),
        np.zeros(70),
    ])
    kernel = np.array([0.1, 0.2, 0.4, 0.2, 0.1])
    freq_smooth = np.convolve(freq_profile, kernel, mode="same")

    phase = np.cumsum(2 * np.pi * freq_smooth / fs)
    env = np.concatenate([
        np.zeros(70),
        np.ones(2350 + len(mod_bits) * sps) * 40.0,
        np.zeros(70),
    ])
    iq_interleaved = np.empty(len(phase) * 2, dtype=np.int8)
    iq_interleaved[0::2] = np.clip(np.round(env * np.cos(phase)), -127, 127).astype(np.int8)
    iq_interleaved[1::2] = np.clip(np.round(env * np.sin(phase)), -127, 127).astype(np.int8)
    iq_interleaved.tofile(filepath)


if __name__ == "__main__":
    targets = sys.argv[1:]
    if not targets:
        targets = sorted(glob.glob("captures/*.complex16s") or glob.glob("*.complex16s"))
    for target in targets:
        print(f"=== {target} ===")
        if os.path.getsize(target) > 100_000:
            decode_multiburst_file(target)
        else:
            bitstream = demodulate_complex16s(target)
            decode_xn297_bits(bitstream)
            print()
