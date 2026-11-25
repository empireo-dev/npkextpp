#!/usr/bin/env python3
# Fits the disk-cost model of docs/tuner_redesign.md to "tune_sim --trace" output:
#   python3 tests/bench/trace_model.py trace.txt [windows per chunk]
"""Model: while writing is the limit, each window's time is
       dt = a_W * files + b * bytes
with one b (seconds per byte: the disk's bandwidth) shared by all writer counts
and a per-file cost a_W for each count W. Fit jointly; then look at how stable
a_W is from short chunks, and the predicted rate for a fixed mix."""
import sys, numpy as np
rows = np.array([[float(x) for x in l.split('trace ')[1].split()] for l in open(sys.argv[1]) if 'trace ' in l])
dt, W, B, F, ww, wi, dw, dwo, rb = rows.T
keep = np.ones(len(rows), bool); keep[:8] = False
for i in range(1, len(rows)):
    if W[i] != W[i-1]:
        keep[i] = False
        if i + 1 < len(rows): keep[i+1] = False
counts = sorted(set(W[keep]))
idx = np.where(keep)[0]
A = np.zeros((len(idx), len(counts) + 1))
for k, i in enumerate(idx):
    A[k, counts.index(W[i])] = F[i]; A[k, -1] = B[i]
x, *_ = np.linalg.lstsq(A, dt[idx], rcond=None)
b = x[-1]
print(f"shared bandwidth {1/b/1e6:.0f} MB/s")
chunk = int(sys.argv[2]) if len(sys.argv) > 2 else 8
for c, w in enumerate(counts):
    j = idx[W[idx] == w]
    a = x[c]
    # a from chunks, given b: (sum dt - b sum B) / sum F
    est = [(dt[j[k:k+chunk]].sum() - b*B[j[k:k+chunk]].sum()) / F[j[k:k+chunk]].sum() for k in range(0, len(j)-chunk+1, chunk)]
    est = np.array(est)
    r64 = 1/(a + b*65536)
    print(f"W={int(w):>2}  a={a*1e6:7.1f} us/file   64 KiB files: {r64:6.0f}/s ({r64*65536/1e6:5.1f} MB/s)   "
          f"a per {chunk/2:.0f} s chunk: cv {est.std()/est.mean():.2f}  {np.round(est*1e6).astype(int)}")
