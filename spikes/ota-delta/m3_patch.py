#!/usr/bin/env python3
"""M3: a head-only retrain, made from M2-new (docs/32#transport, slice 8.0).

usage: m3_patch.py <in.onnx> <out.onnx> [seed]
Overwrites, in place, the raw bytes of the final Gemm's weight and bias initializers with random
values of the same shape, so the file keeps its layout and only the head's bytes change, as a
frozen-backbone fine-tune would.
"""
import sys, numpy as np, onnx
from onnx import numpy_helper

src, dst = sys.argv[1], sys.argv[2]
seed = int(sys.argv[3]) if len(sys.argv) > 3 else 20261015
m = onnx.load(src)
gemm = [n for n in m.graph.node if n.op_type == "Gemm"][-1]
names = set(gemm.input[1:])
rng = np.random.default_rng(seed)
data = bytearray(open(src, "rb").read())
changed, via_bytes = 0, True
for init in m.graph.initializer:
    if init.name not in names:
        continue
    arr = numpy_helper.to_array(init)
    vals = rng.standard_normal(arr.shape).astype(arr.dtype)
    if init.raw_data:
        old = bytes(init.raw_data)
        i = data.find(old)
        assert i >= 0 and data.find(old, i + 1) < 0, f"{init.name}: raw bytes not found exactly once"
        data[i:i + len(old)] = vals.tobytes()
        changed += len(old)
    else:  # stored as float_data: set the values and re-serialise the model below
        via_bytes = False
        del init.float_data[:]
        init.float_data.extend(vals.ravel().tolist())
        changed += vals.nbytes
if via_bytes:
    open(dst, "wb").write(data)
else:
    onnx.save_model(m, dst)
out = open(dst, "rb").read()
diff = sum(1 for a, b in zip(data if via_bytes else open(src, "rb").read(), out) if a != b)
print(f"{dst}: head of {changed} bytes replaced ({', '.join(sorted(names))}); "
      f"{len(out)} bytes, {diff} differ from the source at the same offsets")
