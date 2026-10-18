#!/usr/bin/env python3
"""Deterministic ResNet18 sanity check for the NNP-I SoC CPU runtime.

Usage: resnet_check.py <weights.pth> [num_threads]
Prints reproducible logits summary so host and card runs can be compared.
"""
import sys
import time

import torch
import torchvision

weights = sys.argv[1]
threads = int(sys.argv[2]) if len(sys.argv) > 2 else 4
torch.set_num_threads(threads)

model = torchvision.models.resnet18(weights=None)
model.load_state_dict(torch.load(weights, map_location="cpu"))
model.eval()

gen = torch.Generator().manual_seed(123)
x = torch.randn(1, 3, 224, 224, generator=gen)

with torch.no_grad():
    y = model(x).flatten()

print("torch", torch.__version__, "torchvision", torchvision.__version__, "threads", threads)
print("shape", tuple(y.shape))
print("first5", " ".join("%.6f" % float(t) for t in y[:5]))
print("argmax", int(torch.argmax(y)))
print("sum %.6f mean %.6f std %.6f" % (float(y.sum()), float(y.mean()), float(y.std())))

with torch.no_grad():
    model(x)
t0 = time.time()
with torch.no_grad():
    model(x)
print("infer_ms %.1f" % ((time.time() - t0) * 1000))
