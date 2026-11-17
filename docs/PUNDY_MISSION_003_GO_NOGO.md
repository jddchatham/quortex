# PUNDY MISSION 003 — Stage 1 GO/NO-GO Report: FlashNext First Token

Date: 2026-11-17
Verdict: **GO**
Evidence: `quortex/autonomy/evidence/m-mac-20/` (EVIDENCE.json, REPRODUCE.md,
raw logit vectors, probe logs)

---

## 1. Objective result

The first verified token from the 98.42 GB Qwen3.8-Flash-Next model
(`qwen4exp`, IQ4_XS) was produced on the 16 GB M4 Mac Mini using the pinned
Cortex llama.cpp oracle, under enforceable memory limits, and reproduced
**bit-for-bit** on an independent repeat run.

| Field | Value |
|---|---|
| Prompt | "The capital of France is" (5 tokens incl. BOS) |
| First token | ID **11751**, piece **" Paris"** |
| Logits SHA-256 | `e8effaaba61fb2cf42e3fc05c615e749b5205143d7f7047bad6202c7e70f7650` |
| Determinism | run 2 (cold) vs run 3 (warm): identical hash, identical first-4 floats, identical token |

## 2. Safety gates (all passed)

- Pre-launch: 16 GB RAM, memory 85-91% free, swap 0 used, 22 GiB disk free.
- Enforced kill limits: probe RSS > 10 GB; system memory free < 10%; swap > 1600 MB; runtime > 900 s.
- Observed: peak RSS ~7.26-7.36 GB (mmap-backed), swap 0 throughout, memory pressure 85-91% free.
- Bounded-memory approach: `use_extra_bufts=false` keeps every tensor in the
  mmap-backed CPU buffer; the model is never loaded into RAM (peak touched
  ~7.3 GB of 98.42 GB).
- One token only; run 3 is a determinism repeat of the same single-token run.

Disclosure: the external watchdog's per-process RSS targeting was defective in
all three runs (wrapper-subshell PID in runs 1-2; 15-char `comm` truncation
broke the awk match in run 3). Authoritative per-process numbers come from
`/usr/bin/time -l`; system-level gates (free %, swap) were armed throughout
and never approached. Fixed targeting is queued for the next harness revision.

## 3. Two oracle defects found and fixed at harness level

The pinned library is unchanged. The stock probe harness could not run this
model for two independent reasons, both measured, both worked around in
`native/macos_llama_probe_nr.cpp`:

1. **Weight repack exceeds physical memory.** The stock loader's CPU_REPACK
   buffer type copies repack-eligible tensors into anonymous RAM. For this
   artifact that set includes every `ffn_down_exps` slice (~30 GB total); the
   process was kernel-killed during load at blk.40 of 48 (peak RSS 8.25 GB,
   57.9 s, no GGML error — an OS kill). Fix: `use_extra_bufts=false`.
2. **`embeddings=true` aborts in the qwen4exp graph.** This pinned build
   marks all input positions as outputs when embeddings are requested, then
   reads the output tensor out of bounds
   (`GGML_ASSERT ggml-backend.cpp:423 "tensor read out of bounds"`, SIGABRT).
   Fix: `embeddings=false`; last-position logits suffice for Stage 1.

These are harness parameter adaptations, not engine or library changes, and
they strictly strengthen the memory posture.

## 4. Telemetry (authoritative, from /usr/bin/time -l)

| Metric | Run 2 (cold cache) | Run 3 (warm, determinism) |
|---|---|---|
| Wall / user / sys | 12.07 / 2.92 / 3.73 s | 2.87 / 2.57 / 1.47 s |
| Peak RSS | 7,255,588,864 B (6.76 GiB) | 7,360,741,376 B (6.86 GiB) |
| Page faults | 484,532 | 72,357 |
| Swap used | 0 | 0 |

## 5. Numerical evidence preserved

- `logits0.f32` — raw 248,320-float logit vector (hashes to the fingerprint above).
- `logits0_run3.f32` — independent repeat, SHA-256-identical.
- Full machine-readable record: `quortex/autonomy/evidence/m-mac-20/EVIDENCE.json`.
- Reproduction instructions: `quortex/autonomy/evidence/m-mac-20/REPRODUCE.md`.

## 6. Recommendations for Stage 2 (not started)

- Multi-token trajectories (8-32 tokens) through this same oracle, fingerprints per position.
- Begin the qwen4exp port of the QUORTEX streaming engine, layer-verified
  against this oracle; the preserved logit vector is the first comparison anchor.
- Fix watchdog per-process targeting (match 15-char `comm` prefix).
- Keep the 2 GB-era project guard under review: this model's honest steady-state
  footprint is ~7 GB; an 8 GB guard is the natural ceiling for FlashNext phases.
