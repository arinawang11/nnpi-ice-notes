#!/usr/bin/env python3
"""Deterministic TransformerEncoderLayer sanity check for the card runtime."""
import sys
import time

import torch
import torch.nn as nn

threads = int(sys.argv[1]) if len(sys.argv) > 1 else 2
torch.set_num_threads(threads)

torch.manual_seed(7)
layer = nn.TransformerEncoderLayer(
    d_model=64, nhead=4, dim_feedforward=128, batch_first=True
).eval()
gen = torch.Generator().manual_seed(11)
x = torch.randn(2, 10, 64, generator=gen)

with torch.no_grad():
    y = layer(x)

print("torch", torch.__version__, "threads", threads)
print("shape", tuple(y.shape))
print("first5", " ".join("%.6f" % float(t) for t in y.flatten()[:5]))
print("sum %.6f mean %.6f std %.6f"
      % (float(y.sum()), float(y.mean()), float(y.std())))

with torch.no_grad():
    layer(x)
t0 = time.time()
with torch.no_grad():
    layer(x)
print("infer_ms %.2f" % ((time.time() - t0) * 1000))
