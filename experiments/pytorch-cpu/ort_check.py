#!/usr/bin/env python3
"""Run the exported ResNet18 ONNX model with onnxruntime and compare with torch.

Usage: ort_check.py <model.onnx> <input.pt>
"""
import sys
import time

import numpy as np
import onnxruntime as ort
import torch

model_path, input_path = sys.argv[1], sys.argv[2]
sess = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
x = torch.load(input_path, map_location="cpu")
feed = {sess.get_inputs()[0].name: x.numpy()}
out_name = sess.get_outputs()[0].name

y = sess.run([out_name], feed)[0].ravel()

print("onnxruntime", ort.__version__, "providers", sess.get_providers())
print("first5", " ".join("%.6f" % float(v) for v in y[:5]))
print("argmax", int(np.argmax(y)))
print("sum %.6f mean %.6f std %.6f" % (y.sum(), y.mean(), y.std()))

sess.run([out_name], feed)
t0 = time.time()
sess.run([out_name], feed)
print("infer_ms %.1f" % ((time.time() - t0) * 1000))
