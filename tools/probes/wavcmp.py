#!/usr/bin/env python3
"""wavcmp.py REF.wav PVT.wav --ref-from F0 --frames N [--png OUT.png]

Compares PocketVT's audio (tools/probes/pvtwav, recorded from NES frame F0 for
N frames) with Furbtendulator's (furb_cli --wav, recorded from power-on).  The
reference is cut to the same NES frames (NTSC 60.0988 fps) and both are
stretched to a common length, because PocketVT's audio runs in GBA time: a
cart below 60 NES fps plays proportionally slower.

Reports:
  envelope  correlation of the 10 ms loudness envelopes (rhythm, note on/off)
  spectrum  mean correlation of 50 ms log-magnitude spectra, 60 Hz - 4 kHz
            (pitch and timbre; 1.0 = same notes in the same proportions)
  ltas      correlation of the long-term average spectra (which pitches and
            timbres occur at all; unaffected by tempo)
  level     PocketVT RMS / reference RMS
  speed     PocketVT duration / reference duration (1.0 = full speed)
"""
import argparse
import wave

import numpy as np

NES_FPS = 60.0988


def load(path):
    with wave.open(path) as w:
        n, ch, rate, width = w.getnframes(), w.getnchannels(), w.getframerate(), w.getsampwidth()
        a = np.frombuffer(w.readframes(n), dtype={1: np.uint8, 2: np.int16}[width]).astype(np.float64)
    if width == 1:
        a -= 128
    return a.reshape(-1, ch).mean(axis=1), rate


def resample(a, n):
    return np.interp(np.linspace(0, len(a) - 1, n), np.arange(len(a)), a)


def frames(a, rate, ms):
    step = int(rate * ms / 1000)
    k = len(a) // step
    return a[:k * step].reshape(k, step)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ref'); ap.add_argument('pvt')
    ap.add_argument('--ref-from', type=int, required=True)
    ap.add_argument('--frames', type=int, required=True)
    ap.add_argument('--png')
    a = ap.parse_args()
    ref, rr = load(a.ref)
    pvt, pr = load(a.pvt)
    s0 = int(a.ref_from / NES_FPS * rr)
    ref = ref[s0:s0 + int(a.frames / NES_FPS * rr)]
    speed = (a.frames / NES_FPS) / (len(pvt) / pr)
    rate = 16000
    n = int(a.frames / NES_FPS * rate)
    ref, pvt = resample(ref, n), resample(pvt, n)
    ref -= ref.mean(); pvt -= pvt.mean()

    er = np.sqrt((frames(ref, rate, 10) ** 2).mean(1))
    ep = np.sqrt((frames(pvt, rate, 10) ** 2).mean(1))
    # the two captures start a few ms apart: take the best lag within +-300 ms
    best, lag = -2.0, 0
    for L in range(-30, 31):
        x, y = (er[L:], ep[:len(ep) - L]) if L >= 0 else (er[:L], ep[-L:])
        if x.std() and y.std():
            c = np.corrcoef(x, y)[0, 1]
            if c > best: best, lag = c, L
    env = float(best)
    sh = lag * rate // 100
    if sh > 0: ref, pvt = ref[sh:], pvt[:len(pvt) - sh]
    elif sh < 0: ref, pvt = ref[:sh], pvt[-sh:]

    win = np.hanning(int(rate * 0.05))
    fr, fp = frames(ref, rate, 50), frames(pvt, rate, 50)
    freqs = np.fft.rfftfreq(win.size, 1 / rate)
    band = (freqs >= 60) & (freqs <= 4000)
    cs = []
    for x, y in zip(fr, fp):
        X = np.log1p(np.abs(np.fft.rfft(x * win))[band])
        Y = np.log1p(np.abs(np.fft.rfft(y * win))[band])
        if X.std() > 1e-6 and Y.std() > 1e-6 and (x ** 2).mean() > 100:
            cs.append(np.corrcoef(X, Y)[0, 1])
    spec = float(np.mean(cs)) if cs else float('nan')
    level = float(np.sqrt((pvt ** 2).mean()) / max(np.sqrt((ref ** 2).mean()), 1e-9))
    # long-term average spectrum: WHICH pitches/timbres occur, independent of
    # when -- the fair test when PocketVT runs the cart below full speed
    def ltas(x):
        w = np.hanning(4096); f = np.fft.rfftfreq(4096, 1 / rate); b = (f >= 60) & (f <= 4000)
        acc = sum(np.abs(np.fft.rfft(x[i:i + 4096] * w)) for i in range(0, len(x) - 4096, 2048))
        return np.log1p(acc[b])
    lt = float(np.corrcoef(ltas(ref), ltas(pvt))[0, 1])
    print('envelope %.3f  spectrum %.3f  ltas %.3f  level %.2f  speed %.2f  lag %+d ms  (%d windows)' % (env, spec, lt, level, speed, lag * 10, len(cs)))

    if a.png:   # two spectrograms (0-4 kHz, top = Furbtendulator), no matplotlib needed
        from PIL import Image
        rows = []
        for sig in (ref, pvt):
            w = np.hanning(512)
            cols = [np.log1p(np.abs(np.fft.rfft(sig[i:i + 512] * w))[:128]) for i in range(0, len(sig) - 512, 256)]
            m = np.array(cols).T[::-1]
            rows.append((255 * m / max(m.max(), 1e-9)).astype(np.uint8))
        w = min(r.shape[1] for r in rows)
        img = np.vstack([rows[0][:, :w], np.full((4, w), 255, np.uint8), rows[1][:, :w]])
        Image.fromarray(img).resize((w * 2, img.shape[0] * 2)).save(a.png)


if __name__ == '__main__':
    main()
