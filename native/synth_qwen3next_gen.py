#!/usr/bin/env python3
"""Synthetic qwen3next GGUF generator (M-MAC-15).

Builds a small, fully-F32 qwen3next model with the real architecture's
structure (GDN + full-attention blocks, MoE + shared expert) for three-way
numerical validation without the 48 GB download. Tokenizer KV is replayed
verbatim from the local Qwen2.5-0.5B GGUF so llama.cpp will load it.
"""
import struct, json, sys
import numpy as np

SRC_TOKENIZER_GGUF = "/Users/user/.ollama/models/blobs/sha256-4829649671bf775152642cfbbe771ef8b87a68a94e31a8e8995e1c6e5167edb2"
import os
OUT = os.environ.get("SYNTH_OUT", "/tmp/synth_qwen3next.gguf")
NBLK = int(os.environ.get("SYNTH_NBLK", "4"))
N_EXP = int(os.environ.get("SYNTH_NEXP", "8"))
N_USED = int(os.environ.get("SYNTH_NUSED", "4"))
RECR_OVERRIDE = os.environ.get("SYNTH_RECR")  # comma list e.g. "true" / "false,false"

# ---- synthetic dimensions ----
EMBD = 256
VOCAB = 151936          # keep real vocab size so tokenizer arrays stay valid
HEAD = 4
HEAD_KV = 2
HDIM = 128              # key/value length
ROT = 64                # partial rotary
FFN_EXP = 128
SHEXP = 128
S_STATE = 64
S_GROUP = 4
S_INNER = 512
S_DTR = 8               # dt_rank == num_v_heads
S_HV = S_INNER // S_DTR # 64
CONV = 4
EPS = 1e-6
FBASE = 5e6

rng = np.random.default_rng(20261116)

def rms_w(n):
    return (1.0 + 0.1 * rng.standard_normal(n)).astype(np.float32)

def rand_w(*dims, scale=None):
    w = rng.standard_normal(dims).astype(np.float32)
    if scale: w *= scale
    return w

tensors = {}  # name -> (np.float32 array C-order)

# global
tensors["token_embd.weight"] = rand_w(EMBD, VOCAB, scale=0.02)
tensors["output_norm.weight"] = rms_w(EMBD)
tensors["output.weight"] = rand_w(EMBD, VOCAB, scale=0.02)

def moe_set(p):
    tensors[f"{p}.ffn_gate_inp.weight"] = rand_w(EMBD, N_EXP, scale=0.02)
    tensors[f"{p}.ffn_up_exps.weight"] = rand_w(EMBD, FFN_EXP, N_EXP, scale=0.02)
    tensors[f"{p}.ffn_gate_exps.weight"] = rand_w(EMBD, FFN_EXP, N_EXP, scale=0.02)
    tensors[f"{p}.ffn_down_exps.weight"] = rand_w(FFN_EXP, EMBD, N_EXP, scale=0.02)
    tensors[f"{p}.ffn_gate_inp_shexp.weight"] = rand_w(EMBD, scale=0.02)
    tensors[f"{p}.ffn_gate_shexp.weight"] = rand_w(EMBD, SHEXP, scale=0.02)
    tensors[f"{p}.ffn_up_shexp.weight"] = rand_w(EMBD, SHEXP, scale=0.02)
    tensors[f"{p}.ffn_down_shexp.weight"] = rand_w(SHEXP, EMBD, scale=0.02)

ZERO_BLOCKS = set(int(v) for v in os.environ.get("SYNTH_ZERO_BLOCKS", "").split(",") if v)
recr_list = [((il + 1) % 4 != 0) for il in range(NBLK)]
if RECR_OVERRIDE:
    recr_list = [v.strip() == "true" for v in RECR_OVERRIDE.split(",")]
for il in range(NBLK):
    p = f"blk.{il}"
    tensors[f"{p}.attn_norm.weight"] = rms_w(EMBD)
    tensors[f"{p}.post_attention_norm.weight"] = rms_w(EMBD)
    recr = recr_list[il]
    if recr:
        kd = S_STATE * S_GROUP          # 256
        vd = S_INNER                    # 512
        tensors[f"{p}.attn_qkv.weight"] = rand_w(EMBD, kd * 2 + vd, scale=0.02)
        tensors[f"{p}.attn_gate.weight"] = rand_w(EMBD, vd, scale=0.02)
        tensors[f"{p}.ssm_conv1d.weight"] = rand_w(CONV, kd * 2 + vd, scale=0.05)
        tensors[f"{p}.ssm_dt.bias"] = (0.1 * rng.standard_normal(S_DTR)).astype(np.float32)
        tensors[f"{p}.ssm_a"] = (-np.exp(rng.uniform(-3, 1, S_DTR))).astype(np.float32)
        tensors[f"{p}.ssm_ba.weight"] = rand_w(EMBD, 2 * S_DTR, scale=0.02)
        tensors[f"{p}.ssm_norm.weight"] = rms_w(S_HV)
        tensors[f"{p}.ssm_out.weight"] = rand_w(vd, EMBD, scale=0.02)
    else:
        tensors[f"{p}.attn_q.weight"] = rand_w(EMBD, HEAD * HDIM * 2, scale=0.02)
        tensors[f"{p}.attn_k.weight"] = rand_w(EMBD, HEAD_KV * HDIM, scale=0.02)
        tensors[f"{p}.attn_v.weight"] = rand_w(EMBD, HEAD_KV * HDIM, scale=0.02)
        tensors[f"{p}.attn_output.weight"] = rand_w(HEAD * HDIM, EMBD, scale=0.02)
        tensors[f"{p}.attn_q_norm.weight"] = rms_w(HDIM)
        tensors[f"{p}.attn_k_norm.weight"] = rms_w(HDIM)
    moe_set(p)

for zb in ZERO_BLOCKS:
    for nm in list(tensors.keys()):
        if nm.startswith(f"blk.{zb}."):
            tensors[nm] = np.zeros_like(tensors[nm])

# ---- metadata ----
kv = [
    ("general.architecture", 8, "qwen3next"),
    ("general.name", 8, "synthetic qwen3next (M-MAC-15 validation)"),
    ("general.file_type", 4, 0),          # ALL_F32
    ("general.quantization_version", 4, 2),
    ("qwen3next.block_count", 4, NBLK),
    ("qwen3next.context_length", 4, 4096),
    ("qwen3next.embedding_length", 4, EMBD),
    ("qwen3next.attention.head_count", 4, HEAD),
    ("qwen3next.attention.head_count_kv", 4, HEAD_KV),
    ("qwen3next.attention.key_length", 4, HDIM),
    ("qwen3next.attention.value_length", 4, HDIM),
    ("qwen3next.attention.layer_norm_rms_epsilon", 6, EPS),
    ("qwen3next.rope.dimension_count", 4, ROT),
    ("qwen3next.rope.freq_base", 6, FBASE),
    ("qwen3next.expert_count", 4, N_EXP),
    ("qwen3next.expert_used_count", 4, N_USED),
    ("qwen3next.expert_feed_forward_length", 4, FFN_EXP),
    ("qwen3next.expert_shared_feed_forward_length", 4, SHEXP),
    ("qwen3next.feed_forward_length", 4, 5120),
    ("qwen3next.ssm.conv_kernel", 4, CONV),
    ("qwen3next.ssm.group_count", 4, S_GROUP),
    ("qwen3next.ssm.inner_size", 4, S_INNER),
    ("qwen3next.ssm.state_size", 4, S_STATE),
    ("qwen3next.ssm.time_step_rank", 4, S_DTR),
]
if RECR_OVERRIDE:
    kv.append(("qwen3next.attention.recurrent_layers", 7, recr_list))  # placeholder, replaced below

# ---- replay tokenizer.* KV raw from the qwen2.5 blob ----
FMTSZ = {0:1,1:1,2:2,3:2,4:4,5:4,6:4,7:1,10:8,11:8,12:8}
f = open(SRC_TOKENIZER_GGUF, "rb")
f.read(4); ver, n_t, n_kv = struct.unpack("<IQQ", f.read(20))
def rstr():
    n, = struct.unpack("<Q", f.read(8)); return f.read(n)
tok_kv_raw = []
for _ in range(n_kv):
    k = rstr().decode()
    t, = struct.unpack("<I", f.read(4))
    if not k.startswith("tokenizer."):
        # skip value
        if t == 8:
            n, = struct.unpack("<Q", f.read(8)); f.seek(n, 1)
        elif t == 9:
            et, n = struct.unpack("<IQ", f.read(12))
            if et == 8:
                for _ in range(n):
                    ln, = struct.unpack("<Q", f.read(8)); f.seek(ln, 1)
            else: f.seek(FMTSZ.get(et, 8) * n, 1)
        else:
            f.seek(FMTSZ[t], 1)
        continue
    if t == 8:
        n, = struct.unpack("<Q", f.read(8)); v = f.read(n)
    elif t == 9:
        et, n = struct.unpack("<IQ", f.read(12))
        raw = f.read(FMTSZ.get(et, 8) * n) if et != 8 else b"".join(
            struct.pack("<Q", (ln := struct.unpack("<Q", f.read(8))[0])) + f.read(ln) for _ in range(n))
        v = (et, n, raw)
    else:
        v = f.read(FMTSZ[t])
    tok_kv_raw.append((k, t, v))
f.close()
print(f"replayed {len(tok_kv_raw)} tokenizer kv entries")

# ---- write GGUF ----
def kv_bytes(k, t, v):
    out = struct.pack("<Q", len(k)) + k.encode() + struct.pack("<I", t)
    if t == 8:
        out += struct.pack("<Q", len(v)) + v
    elif t == 9:
        et, n, raw = v
        out += struct.pack("<IQ", et, n) + raw
    else:
        out += v
    return out

def meta_num(k, t, x):
    return (k, t, {0:struct.pack("<B",x),4:struct.pack("<I",x),5:struct.pack("<i",x),6:struct.pack("<f",x),12:struct.pack("<q",x)}[t])

body_kv = b""
for k, t, v in kv:
    if t == 8: body_kv += kv_bytes(k, t, v.encode())
    elif t == 4: body_kv += kv_bytes(k, t, struct.pack("<I", v))
    elif t == 6: body_kv += kv_bytes(k, t, struct.pack("<f", v))
    elif t == 7:  # bool array
        out = struct.pack("<Q", len(k)) + k.encode() + struct.pack("<I", 9)  # GGUF_KV_ARR
        out += struct.pack("<I", 7) + struct.pack("<Q", len(v))
        out += b"".join(struct.pack("<?", x) for x in v)
        body_kv += out
for k, t, v in tok_kv_raw:
    body_kv += kv_bytes(k, t, v)

order = list(tensors.keys())
infos = b""
data = b""
off = 0
for nm in order:
    a = np.ascontiguousarray(tensors[nm], dtype=np.float32)
    nd = len(a.shape)
    dims = a.shape
    # ggml flat order indexes ne0 fastest; numpy C-order indexes it slowest.
    # Write axes reversed so llama's logical tensor == the numpy array.
    if a.ndim > 1:
        a = np.ascontiguousarray(a.transpose(*reversed(range(a.ndim))), dtype=np.float32)
    b = a.tobytes()
    infos += struct.pack("<Q", len(nm)) + nm.encode()
    infos += struct.pack("<I", nd)
    for d in dims: infos += struct.pack("<q", d)
    infos += struct.pack("<IQ", 0, off)   # type F32, offset
    data += b
    off += len(b)

data_off = 4 + 4 + 8 + 8 + len(body_kv) + len(infos)
data_off = (data_off + 31) // 32 * 32
pad = data_off - (4 + 4 + 8 + 8 + len(body_kv) + len(infos))
hdr = b"GGUF" + struct.pack("<IQQ", 3, len(order), len(kv) + len(tok_kv_raw))
out = hdr + body_kv + infos + b"\0" * pad + data
with open(OUT, "wb") as g:
    g.write(out)
print(f"wrote {OUT}: {len(out)} bytes, {len(order)} tensors, data_off={data_off}")
# save the weights for the numpy reference
np.savez(os.environ.get("SYNTH_NPZ", "/tmp/synth_qwen3next_weights.npz"), **tensors)
print("weights npz saved")
