#!/usr/bin/env python3
"""Generate the bundled 48k/2ch/16bit sample WAV (20s gentle pink noise)."""
import array
import os
import random
import struct
import wave

OUT = os.path.join(os.path.dirname(__file__), "..", "app", "src", "main", "assets", "sample", "48k_2ch_16bit.wav")

SAMPLE_RATE = 48000
CHANNELS = 2
BITS = 16
DURATION = 20.0
PEAK = 0.25  # 归一化峰值振幅，压低保证不刺耳
FADE = 0.02  # 20ms 淡入淡出，避免噪声起始/结束爆音
SEED = 20260827  # 与 AudioTester 同种子 → 内容一致

rng = random.Random(SEED)
n = int(SAMPLE_RATE * DURATION)

# 第一遍：生成浮点粉噪并记录峰值（Paul Kellett 6 极点近似，-3dB/oct）
frames = array.array('f')
b0 = b1 = b2 = b3 = b4 = b5 = b6 = 0.0
peak = 0.0
for _ in range(n):
    w = rng.uniform(-1.0, 1.0)
    b0 = 0.99886 * b0 + w * 0.0555179
    b1 = 0.99332 * b1 + w * 0.0750759
    b2 = 0.96900 * b2 + w * 0.1538520
    b3 = 0.86650 * b3 + w * 0.3104856
    b4 = 0.55000 * b4 + w * 0.5329522
    b5 = -0.7616 * b5 - w * 0.0168980
    v = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362
    b6 = w * 0.115926
    a = abs(v)
    if a > peak:
        peak = a
    frames.append(v)

# 第二遍：归一化到 PEAK，加淡入淡出，写 16bit PCM int
gain = PEAK / peak
MAX_I16 = 32767
fade_samples = int(FADE * SAMPLE_RATE)
buf = bytearray(n * CHANNELS * (BITS // 8))
fmt = struct.Struct("<hh")  # 立体声，双声道同值
for i in range(n):
    env = 1.0
    if i < fade_samples:
        env = i / fade_samples
    elif i > n - fade_samples:
        env = (n - i) / fade_samples
    sample = int(frames[i] * gain * env * MAX_I16)
    fmt.pack_into(buf, i * 4, sample, sample)

os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)
with wave.open(OUT, "wb") as w:
    w.setnchannels(CHANNELS)
    w.setsampwidth(BITS // 8)
    w.setframerate(SAMPLE_RATE)
    w.writeframes(bytes(buf))
print(f"Generated {OUT}: {os.path.getsize(OUT)} bytes, peak={peak:.3f}")
