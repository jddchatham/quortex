#!/usr/bin/env python3
"""Compare QUORTEX and Cortex Segment-A/Segment-B capture directories."""
import argparse, hashlib, json, math, struct, sys
from pathlib import Path

BOUNDARY = 12804
ROUTER = 512
POST_SEGMENT_B = 10240
PRE_HC = 2560

def read(path, count):
    raw = path.read_bytes()
    if len(raw) != count * 4:
        raise ValueError(f"{path}: expected {count*4} bytes, got {len(raw)}")
    vals = struct.unpack("<%df" % count, raw)
    if not all(math.isfinite(x) for x in vals):
        raise ValueError(f"{path}: non-finite value")
    return raw, vals

def sha(raw): return hashlib.sha256(raw).hexdigest()

def component(a, b):
    diffs = [abs(x-y) for x,y in zip(a,b)]
    scale = [max(abs(y), 1e-30) for y in b]
    return {"max_abs": max(diffs), "rms": math.sqrt(sum(x*x for x in diffs)/len(diffs)),
            "max_rel": max(d/s for d,s in zip(diffs, scale)),
            "max_abs_index": max(range(len(diffs)), key=lambda i: diffs[i]),
            "bitwise": a == b}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference", type=Path); ap.add_argument("quortex", type=Path)
    ap.add_argument("--max-abs", type=float, default=1e-5)
    ap.add_argument("--max-rel", type=float, default=1e-5)
    ap.add_argument("--manifest", type=Path)
    args = ap.parse_args()
    result = {"version": 1, "format": "little-endian IEEE-754 F32", "status": "INCONCLUSIVE", "components": {}}
    try:
        for name, count in (("segment_a_boundary", BOUNDARY),
                            ("router_logits", ROUTER),
                            ("pre_hc_block_out", PRE_HC),
                            ("post_segment_b", POST_SEGMENT_B)):
            rr, rv = read(args.reference / f"cortex_{name}_p0.bin", count)
            qr, qv = read(args.quortex / f"quortex_{name}_p0.bin", count)
            result["components"][name] = {"elements": count, "sha256_reference": sha(rr),
                "sha256_quortex": sha(qr), **component(rv, qv)}
        passed = all(x["bitwise"] or (x["max_abs"] <= args.max_abs and x["max_rel"] <= args.max_rel)
                     for x in result["components"].values())
        result["status"] = "PASS" if passed else "FAIL"
    except (OSError, ValueError) as e:
        result["reason"] = str(e)
    out = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.manifest: args.manifest.write_text(out)
    print(out, end="")
    return 0 if result["status"] == "PASS" else 1

if __name__ == "__main__": sys.exit(main())
