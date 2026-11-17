# PUNDY MISSION 001 - QUORTEX Independent Investigation Report

Date: 2026-11-17
Author: Pundy (autonomous investigation)
Scope: read-only investigation, baseline verification against evidence, FlashNext preparation.
No model files, downloads, or Git branches were modified. No inference was run.

---

## 0. Executive summary

- QUORTEX is a disciplined, evidence-first research project with a working
  low-memory streaming inference engine for Qwen3-Next (`qwen3next`) models.
- The verified milestone `backup/m-mac-18-low-memory` (2.26 tok/s, 632 MB RSS)
  and its successor commit M-MAC-19 on `dev/macos-arm64-qwen3next`
  (2.90 tok/s warm, 1.80 GB RSS) are **supported by preserved, internally
  consistent machine-readable evidence**. I accept both numbers as recorded.
- The incoming model `Qwen3.8-Flash-Next-Uncensured.IQ4_XS.gguf` is a
  **different architecture**: `qwen4exp` (Qwen3.8 Flash Next). It cannot run on
  the QUORTEX custom streaming engine today. It **can** run through the pinned
  Cortex llama.cpp library that is already built on this machine (the built
  `libllama.0.5.0.dylib` contains `llama_model_qwen4exp`), once the download
  completes and passes size + SHA-256 verification.
- The three biggest bottlenecks: (1) per-token expert I/O of ~1.30 GB against a
  66 GB expert region that does not fit page cache; (2) the fp32 exactness tax
  in the MoE path (measured ~34% combined); (3) a serial, synchronous
  read-then-compute pipeline with no async overlap across layers.
- The most promising breakthrough: **speculative batch verification with a
  small resident draft model** (bit-exact under greedy decoding), which
  parallelizes the dominant expert I/O across positions. Near-term gains come
  from async expert prefetch plus quant-native expert kernels.

---

## 1. What QUORTEX currently does

### 1.1 Project shape

Repository `~/workspace/quortex-macos`, four local branches:

| Branch | Role |
|---|---|
| `backup/m-mac-18-low-memory` | Preserved verified milestone (M-MAC-18) |
| `quortex` | Main research line |
| `dev/macos-arm64-prep` | macOS preparation line |
| `dev/macos-arm64-qwen3next` | HEAD; M-MAC-19 accepted here (+2 commits over backup) |

`remotes/origin/main`, `remotes/origin/quortex` exist; nothing was pushed.

Two cooperating layers:

1. **Python autonomy/evidence framework** (`quortex/`, `tests/`, 104 test
   functions, stdlib `unittest`): a pre-inference contextual gate
   (`quortex/cli.py`, quadrant state machine in `quortex/quadrant.py`), a
   bounded supervisor/director (`quortex/autonomy/`), and a machine-readable
   evidence convention (`quortex/autonomy/evidence/m-mac-NN/EVIDENCE.json`).
   Research discipline (from `README.md`): claims tagged measured / derived /
   hypothesized / unknown; every optimization keeps an independently testable
   oracle; models and binaries stay out of git.

2. **Native C++ streamed inference engine** (`native/`): the macOS work is
   `native/macos_qwen3next_stream.cpp` (1093 lines). It implements the
   Qwen3-Next architecture by hand on top of ggml:
   - Gated DeltaNet (GDN) recurrence with carried conv/ssm state;
   - gated GQA full attention every 4th layer with fp32 KV cache (CTX 8192);
   - 512-expert MoE, top-10 plus a shared expert, per-token expert slices
     streamed with `pread` into reusable staging buffers;
   - persistent 6-thread ggml threadpool; per-layer batched graphs
     (`qmatvec_multi`);
   - bit-exact NEON Q4_K dequant (`dq4K_neon`, validated at runtime against
     the ggml reference before first use);
   - M-MAC-19 hybrid zero-copy: static weights are read directly from a
     `MAP_SHARED` mapping (ggml aliases file pages, no staging memcpy); expert
     slices and the LM head stay on the bounded `pread` staging path;
   - kernel-driven readahead (`madvise(MADV_WILLNEED)`) of the next layer's
     static weights - prefetch without RSS growth.

   Supporting native tools: `macos_llama_probe` / `macos_llama_session`
   (deterministic, fingerprint-emitting harnesses over the pinned Cortex
   llama library - the trusted oracle), `macos_stream_decode` (bounded
   whole-model streaming decode), `synth_qwen3next_gen.py` (synthetic GGUF
   generator), `gguf_diagnostic`, `dequant_oracle`.

### 1.2 External dependencies

- `~/workspace/cortex-build-src`: pinned llama.cpp/ggml source snapshot
  (ggml 0.25.3, libllama 0.5.0), no git metadata. Its `AGENTS.md` is the
  upstream llama.cpp contributor policy (relevant only if patching llama.cpp;
  we are not).
- `~/workspace/cortex-build-mac`: built shared libraries only
  (`libllama`, `libggml*`, BLAS). No `llama-cli` binary exists on this Mac;
  oracle access is via the probe binaries above.

---

## 2. What is actually verified

Verified = preserved evidence (EVIDENCE.json + git commits) that is internally
consistent and, where stated, cross-checked against the independent llama.cpp
oracle. I verified the *evidence*, not the runs (re-running was out of scope).

| Milestone | Claim | Evidence | Assessment |
|---|---|---|---|
| M-MAC-15 | Real-weight streaming inference matches llama.cpp exactly on qwen3next | `m-mac-15/EVIDENCE.json`, synthetic 3-way + real-weight streams | Accepted |
| M-MAC-16 | Context 64 -> 8192; streaming adapter profiled/optimized | commit `5dab5d5` | Accepted |
| M-MAC-17 | Quant-native matvec, batched expert graphs, persistent threadpool | `3229b17`, `m-mac-17/EVIDENCE.json` | Accepted |
| M-MAC-18 | 2.26 tok/s warm, RSS 632 MB; bit-exact NEON Q4_K gate | `backup/m-mac-18-low-memory` @ `3525685`, evidence commit `85bb97d` | Accepted (preserved branch) |
| M-MAC-19 | Hybrid zero-copy: 2.90 tok/s warm (runs 2.897/2.909/2.896), cold 1.27, peak RSS 1.80 GB, token-exact | `1a28be7` + `46a6a2f`, `m-mac-19/EVIDENCE.json` | Accepted |
| Gate LRU | **Rejected on measurement**: >=40% hit needs >=2.4 GB (> cap); at cap, <=13% | `m-mac-19/EVIDENCE.json` | Accepted rejection |
| Full zero-copy | **Rejected**: 3.66 tok/s but RSS 7.1 GB | same | Accepted rejection |
| M-MAC-12..14 | Layer-0 exactness, multi-token fp64 reference, independent validation | commit log + evidence | Accepted as historical |

Notable integrity properties of the evidence chain: preserved streams
(`9707` x4, `100000`) re-checked across modes; a real bug (phase-A up-projection
pointer regression) was caught and fixed by these streams; RSS accounting
honestly charges `MAP_SHARED` resident pages to the process. The evidence
culture here is genuinely good - keep it.

Open items noted (not blocking): evidence dirs `m-mac-01..12` and
`QIES-060-TWO-TOKEN.json` are untracked; `native/build-mac/` is untracked
build output; a stale `quortex/autonomy/inference.darwin.lock` exists;
downloaded artifacts from an earlier abandoned Q3_K_L/Q3_K_M fetch (~52 GB)
still sit in `~/Downloads/Qwen3.8-Flash-Next-Uncensored-GGUF/` - I did not
touch them.

---

## 3. FlashNext: what it is, and whether it can run

### 3.1 Download status (observed, not interfered with)

- File: `~/Downloads/Qwen3.8-Flash-Next-Uncensored.IQ4_XS.gguf.part`
- Expected (pinned in `~/workspace/fetch-flashnext.sh`): **98,421,788,672 bytes**,
  **SHA-256 `a1975fd73542230e2b344d112f29f22ff522d051cb4bf33bdb872cb3ffc5dedf`**
- Observed: 92,680,609,792 B at 12:55 MST -> 94,574,518,272 B at 13:02 MST
  (~4.5 MB/s, actively writing). ~3.85 GB remain; ETA roughly 15 min at the
  observed rate.
- Disk: 13 GiB free now; completion needs ~3.6 GiB - fits, but leaves the
  system tight. Do not start other large downloads.
- **Hard gate: no read beyond the header, no inference, until the fetch script
  renames to final and the SHA-256 verifies.**

### 3.2 Model anatomy (parsed live from the GGUF header of the partial file)

`general.architecture = qwen4exp`, "Qwen3.8 Flash Next Uncensored"
(mradermacher, `convert_type=hf`). 1,224 tensors, **176.9B elements,
~98.41 GB** (independent byte tally matches the pinned size).

| Property | Value |
|---|---|
| Layers | 48: 36 Gated DeltaNet + 12 full attention (every 4th, `full_attention_interval=4`) |
| Embeddings | 2560; vocab **248,320** |
| MoE | 512 experts, top-10 + 1 shared; expert FFN 640 |
| GDN | ssm state 128, inner 6144, dt-rank 48 (=> 128 v-heads), conv 4, groups 16 |
| Full attn | 24 heads / 2 KV, head dim 256, QK-norm, RoPE base 1e7 |
| QSA indexer | on attn layers: 4 indexer heads, key 128, **top_k 2048** tokens |
| Hyper-connections | 4 streams, low-rank 320 (`hc_attn_*`, `hc_ffn_*`, `output_hc_*`) |
| PLE | per-layer n-gram position embeddings: n-gram 3, 16 heads (`head_offsets`/`head_vocab_sizes` x16), conv kernel 4 |
| Per-layer token embd | 160-dim rows (`per_layer_token_embd.weight`) |
| Quant mix | IQ4_XS 562, IQ4_NL 182, Q5_K 54, Q5_1 12, TQ2_0 24, F32 388, Q6_K 1, F16 1 |

Byte budget (exact, from header tensor offsets):

| Region | GB | Note |
|---|---|---|
| Expert tensors (48 x 3) | 66.37 | gate/up IQ4_XS, down Q5_1 |
| PLE lookup table | 28.80 | 51.2B x 160-dim rows - sparse random row reads per token |
| Attention + indexer | 1.31 | |
| Embeddings + LM head | 0.86 | head is Q6_K, 521 MB |
| Shared FFN + router | 0.38 | |
| Hyper-connections | 0.35 | |
| GDN | 0.31 | |
| **Total** | **98.41** | |

Derived operational facts:
- **Expert working set per token: ~1.30 GB** (top-10 x 3 tensors x 48 layers,
  2.97 MB/expert). vs ~0.8 GB/token on the Huihui qwen3next model that
  produced the 2.90 tok/s baseline.
- Static (non-expert, always-resident) weights: ~3.2 GB - over twice the
  M-MAC-19 mapped footprint (1.1 GB). An honest RSS for FlashNext short-ctx
  runs is ~5-6 GB, i.e. the 2 GB project guard needs renegotiation for this
  phase.
- GDN state ~113 MB + conv ~4 MB; KV cache tiny at validation lengths.
- LM head matvec: 248,320 x 2560 Q6_K = 521 MB read per token (paged after
  first touch).

### 3.3 Can QUORTEX run it?

**Custom streaming engine (`macos-qwen3next_stream.cpp`): no, not today.**
Blocking gaps, in order of size:

1. Hyper-connections (4-stream residual topology) - entirely unimplemented.
2. QSA indexer attention - unimplemented (for ctx <= 2048 top-k it is near-dense,
   but the indexer scores still select blocks and must be computed exactly).
3. PLE n-gram per-layer position embeddings - unimplemented.
4. Per-layer 160-dim token embeddings - unimplemented.
5. GDN parameterization changed: `ssm_alpha`/`ssm_beta` static weights replace
   the `ssm_ba` projection; inner size 6144 (vs 4096), dt-rank 48 (vs 32).
6. Metadata keys are `qwen4exp.*`; engine reads `qwen3next.*` (all lookups
   silently default).
7. Quant kernels: only Q4_K has a NEON fast path; this model is mostly
   IQ4_XS/IQ4_NL/Q5_1/TQ2_0 (ggml fallback works but is the slow path).

**Pinned llama.cpp oracle: yes.** The built `libllama.0.5.0.dylib` (2026-11-15)
contains `llama_model_qwen4exp`, `graph_mtp`, `llm_graph_input_qwen4exp_ple`,
and all `hc_attn_inject` / `indexer.k_proj` / `per_layer_token_embd` tensor
handling. The existing probe/session harness pattern can therefore run
FlashNext as the trusted reference (bounded tokens, greedy, logit SHA-256
fingerprints) subject to file verification and an adjusted RSS guard.

---

## 4. The three biggest architectural bottlenecks

Ranked by expected impact on FlashNext decode, grounded in the M-MAC-19
measurements and the model anatomy above.

**B1. Expert I/O against an un-cacheable working set.**
~1.30 GB/token read from a 66 GB expert region inside a 98 GB file. Measured
reuse is weak: 44.2% previous-token hit, 58.5% unbounded-session hit
(M-MAC-19 `Q3N_CACHE_STATS`), and a per-layer LRU needs >=2.4 GB to reach 40%
- rejected. On a 16 GB host, page cache cannot hold the union of experts used
across a session, so SSD random reads dominate: at the ~2.3 GB/s effective
rate implied by the 2.90 tok/s Huihui result, the raw ceiling here is
~1.7 tok/s before compute. **This is the bottleneck the previous architecture
has not beaten; it is structural to top-10-of-512 routing.**

**B2. The fp32 exactness tax in the MoE path.**
Measured on M-MAC-19: expert-gate dequant (NEON Q4_K) ~19% + gate fp32
`vec_dot` ~15%. The gate slices are dequantized to fp32 and run as fp32
matmuls because q8_K x-quantization flipped the preserved verification
streams. That choice bought bit-exactness and costs roughly a third of the
compute budget. The same tax will apply to FlashNext (larger layers, same
structure) until a re-verified quant-native formulation exists.

**B3. Serial, synchronous read-then-compute per layer.**
Within a layer the engine batches graphs well, but across layers it strictly
alternates read / compute / read, with only `madvise` readahead (best-effort,
no completion tracking, no RSS cost). There is no async double-buffered tile
engine (genesis hypothesis H6): expert slices for layer l are not fetched
while layer l-1 computes, and head/output work is serialized after layer 47.
Every millisecond of read latency is exposed on the critical path.

(Honorable mention: the 521 MB Q6_K LM head read per token is 40% of one
token's expert traffic; paged it amortizes, but it deserves batched/verified
treatment too.)

---

## 5. Most promising breakthrough opportunity

**Speculative batch verification with a small resident draft model
(genesis H3), executed bit-exactly under greedy decoding.**

Why this and not another kernel: per-token cost on FlashNext is dominated by
~1.30 GB of *mandatory* expert bytes (B1). Kernels (B2) and overlap (B3)
attack the constants; speculation attacks the structure. With a compact draft
(e.g. a ~0.5-1 GB quant small model) proposing k tokens, one target forward
pass over k positions verifies them; under greedy argmax the accepted prefix
is exactly what sequential decoding would produce - no approximation, no new
correctness class, and the preserved-stream oracle still applies. The crucial
mechanism for this workload: all k positions' expert routings are known after
the draft, so their ~1.30 GB of reads issue **concurrently** (deep SSD queue)
instead of as k serial rounds. I/O-bound decode converts partially into
bandwidth-bound decode - an expected 1.5-2.5x on top of B2/B3 fixes, which no
purely local optimization can reach. The model artifact carries no MTP weights
(tensor survey shows none), so the draft must be external; draft acceptance
must be measured, never assumed (acceptance rate is the one true risk).

Near-term, independently valuable (feasible within existing verification
machinery): async double-buffered expert prefetch (H6) + quant-native
IQ4_XS/Q5_1 expert kernels with token-exact stream verification (relaxing
bit-exactness to argmax-trajectory-exactness, documented as a new semantic
mode).

Approaches considered and deprioritized, with reasons:
- *Metal GPU offload*: unified memory conflicts with the streaming design
  (GPU wants resident buffers); ggml BLAS (Accelerate) already covers fp32.
  Revisit only after B1-B3, for the GDN recurrence.
- *Offline weight repacking (H2)*: valid but the GGUF layout is already
  slice-contiguous per expert; gains are second-order vs B1-B3.
- *Surrogate/low-rank predictors (H4/H5)*: different model class; violates
  the exactness mission. Not now.
- *Output-directed certification (H7)*: no sound certificate known. Research
  only.

---

## 6. Staged implementation and testing plan

Gates before anything: (a) download completes and fetch script verifies
SHA-256; (b) user authorizes leaving pure read-only mode; (c) RSS guard for
FlashNext phases set explicitly (recommend 8 GB ceiling, 0 swap, fail-closed);
(d) every stage produces an `EVIDENCE.json` in the existing convention and
preserves the M-MAC-18 backup branch untouched.

**Stage 0 - File verification (no inference).**
Confirm final size 98,421,788,672 and SHA-256
`a1975fd7...dedf` via the fetch script. Parse header into a pinned metadata
snapshot (tensor offsets, types, dims) for regression tests. Deliverable:
verification record. (~minutes)

**Stage 1 - Oracle qualification (bounded, cheap).**
Extend `macos_llama_probe` usage: load FlashNext via the pinned libllama,
decode 1-4 tokens greedy on a fixed prompt, emit logit SHA-256 fingerprints +
RSS + load time. Expect model load traffic ~98 GB page-in but RSS bounded by
guard; first-token latency dominated by weight reads - record it as the
reference TTFT. Deliverable: FlashNext oracle baseline evidence. (~1 hour)

**Stage 2 - qwen4exp port of the streaming engine (the bulk).**
Sub-stages, each verified against the Stage-1 oracle with existing trace-dump
machinery (`Q3N_TRACE0`-style per-layer captures):
2a. Metadata + GDN layers with `ssm_alpha/ssm_beta`, inner 6144, dt-rank 48;
2b. hyper-connections (4-stream topology) on GDN layers;
2c. full-attention layers with QSA indexer (exact block selection for ctx<=2048);
2d. PLE + per-layer token embeddings (sparse row reads via the zero-copy map);
2e. LM head Q6_K path + output hyper-connections.
Deliverable: 8-32 token trajectories bit-matching the oracle on both GDN and
attn-heavy prompts. (Largest stage; incremental, low risk per sub-stage.)

**Stage 3 - Performance, measured and gated.**
3a. Async double-buffered expert prefetch (bounded buffers, completion-tracked);
3b. quant-native IQ4_XS/Q5_1 expert kernels behind a flag, accepted only if
preserved streams stay token-exact (documented semantic mode);
3c. head-read amortization.
Each change: median-of-3 warm + 1 cold 12 GB-eviction run, RSS guard, byte
counters. Target: beat 1.7 tok/s implied I/O ceiling, then push toward
page-cache-assisted rates. Deliverable: EVIDENCE.json per accepted change.

**Stage 4 - Speculative verification (breakthrough track).**
Add a resident draft backend + batched k-position verify pass with concurrent
expert reads. Measure acceptance rate, tok/s, RSS; accept only if
token-exact vs oracle on preserved streams. Deliverable: accept/reject with
numbers.

**Risks and watch items:** draft acceptance unknown until measured (S4);
QSA indexer exactness at ctx>2048 needs block-selection tests (defer until S2c
short-ctx is exact); disk headroom (~9 GiB post-download) limits auxiliary
artifacts; macOS charges MAP_SHARED pages to RSS - keep the M-MAC-19 honest
accounting; the abandoned Q3_K partials (~52 GB) are user property - propose,
don't delete.

---

## Appendix - claims ledger

| Claim | Status |
|---|---|
| M-MAC-18: 2.26 tok/s warm, 632 MB RSS | Measured (preserved evidence), accepted as recorded |
| M-MAC-19: 2.90 tok/s warm, 1.80 GB RSS, +28% | Measured (preserved evidence), accepted as recorded |
| Gate LRU ineffective at 2 GB | Measured (M-MAC-19), accepted |
| FlashNext = qwen4exp, 48L (36 GDN + 12 attn), 512-expert top-10 MoE, vocab 248320, 98.41 GB | Measured (parsed header + offset math) |
| libllama on this Mac can load qwen4exp | Measured (symbols in built dylib) |
| Expert traffic ~1.30 GB/token | Derived (exact tensor extents) |
| FlashNext ceiling ~1.7 tok/s from I/O before compute | Derived (from measured Huihui rate + exact bytes) |
| Speculative verify yields 1.5-2.5x | Hypothesized - must be measured in S4 |
| IQ4_XS/Q5_1 quant-native path is faster and stays token-exact | Hypothesized - S3b gate |
