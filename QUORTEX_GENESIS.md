# QUORTEX — Genesis

Status: initial research brief, 2026-10-22

This document establishes the project before major implementation. It is deliberately skeptical: the mission is to discover whether radically faster inference is possible, not to convert an ambition into a performance claim.

## 1. Verified baseline inherited from Cortex

### Reference conditions

- Repository: [jddchatham/cortex-stream-forge](https://github.com/jddchatham/cortex-stream-forge)
- Local reference clone: `/home/user/bitnet-cortex/cortex-stream-forge`
- Upstream milestone observed locally: commit `8da466d`, tag `cortex-first-token-milestone`
- Reference host: AMD Athlon Silver 3050U
- Target model: approximately 90.78 GiB Qwen FlashNext GGUF; the exact artifact identity and hash must be recorded before any full-model benchmark

### Verified engineering facts

The Cortex notes and frozen checkpoints establish these facts, with scope limited to the named tests and model/runtime variants:

1. GGUF metadata and tensor descriptors can be indexed without mapping the complete payload.
2. Selected tensor ranges can be read with `pread` into a reusable bounded layer buffer.
3. A streamed layer transaction can keep global data, activations, recurrent state, and KV state in separate lifetimes.
4. A frozen Flash-Next transaction selected experts dynamically and recorded zero unselected-expert reads in that transaction. This is transaction evidence, not full-model generation equivalence.
5. A layer-0 stock-versus-Cortex numerical regression recorded exact recurrent state at the specified boundary, max absolute error `1.4901161e-08`, RMS error `5.7019197e-10`, and no unselected expert reads.
6. A Qwen3.8 low-residency provider measured approximately 284 MiB RSS growth for its provider probe, with a 252,263,296-byte layer buffer and a 1,758,126,080-byte global/final-weight buffer. This is not evidence of instant inference or complete chat equivalence.
7. The measured checkpoint-resume profile recorded `wall_sec=120.129292`, `decode_sec=5.064467`, `sparse_moe_sec=18.478420`, `model_load_sec=39.636310`, `state_compare_sec=23.825337`, `pread_calls=327712`, and `pread_bytes=1927710720` for one step. The exact benchmark command and checkpoint lineage must be preserved with future reruns.

### Baseline interpretation

Cortex proves bounded residency and several important correctness boundaries. It does not prove low latency. The dominant measured costs include startup/loading, state materialization and comparison, sparse expert I/O, and repeated small reads. QUORTEX therefore treats “stream the same computation more efficiently” as only one hypothesis among several.

The local Cortex checkout is dirty with later experiments and generated build trees. It is preserved unchanged. QUORTEX currently imports no Cortex source, weights, checkpoints, binaries, or unreviewed working-tree changes.

## 2. Fundamental obstacles

1. **Information movement:** a model with tens of gigabytes of parameters cannot be consulted arbitrarily without moving information. Storage latency, page faults, filesystem scheduling, and CPU copies can dominate arithmetic.
2. **Arithmetic volume:** dense transformer layers require large matrix-vector products even when the batch is one. Quantization lowers bytes and arithmetic cost but does not remove the dependency structure.
3. **State dependency:** recurrent state, attention/KV state, normalization, routing, and residual paths constrain parallelism across layers and tokens.
4. **Sparse routing is conditional:** router decisions can avoid expert traffic only after the router computation is complete. A graph that registers all experts may erase the benefit.
5. **Cold-start cost:** time to first token includes metadata, model opening, graph construction, allocation, and state restoration; subsequent-token speed cannot hide it.
6. **Correctness sensitivity:** small numerical changes can alter top-k routing or token choice. “Similar answers” is not model equivalence.
7. **Bandwidth lower bounds:** if the answer depends on a large fraction of weights, a storage-traffic lower bound may make near-instant exact inference impossible on the reference machine.

## 3. Architectural hypotheses

These are competing hypotheses, not claims.

### H1 — Router-first, selected-expert streaming

Split each token step into a cheap shared/recurrent/router phase, then fetch only selected expert slices with coalesced asynchronous reads. This extends Cortex’s strongest evidence. It could provide large gains when expert selection is highly sparse and expert reads are the bottleneck; it cannot beat the cost of the dense shared path or eliminate routing latency.

### H2 — Offline transformed streaming representation

Transform weights offline into token-step-oriented blocks: fused quantized projections, locality-aware expert shards, precomputed scales/permutations, and storage-aligned tiles. The runtime reads exactly the tiles needed by a kernel, avoiding GGUF tensor reconstruction and small-read overhead. This could yield orders-of-magnitude improvement over pathological I/O, but not over the information required by an exact dense computation.

### H3 — Predictive/speculative computation with rollback

Use a small resident predictor, cached layer summaries, or a draft model to propose several tokens or routing paths; verify them against the large model and roll back on disagreement. This can make interactive output feel near-instant when acceptance is high. It is approximate until every accepted token is verified, and worst-case latency remains the large-model cost.

### H4 — Learned low-rank/state-space surrogate

Replace repeated large-model computation with a compact resident dynamical system trained or fitted offline to predict hidden-state updates, invoking the full model only on uncertainty-triggered checkpoints. This is plausibly orders faster, but it is a different model unless an exact proof exists; evaluation must measure fidelity, calibration, and fallback frequency.

### H5 — Tensor-network / factorized contraction

Offline factorize matrices and represent the computation as a tensor network with contraction order chosen for one-token execution. Structured low-rank or tensor-train factors may reduce arithmetic and traffic. The risk is rank inflation, routing discontinuities, and approximation error on the actual model rather than a toy matrix.

### H6 — Persistent graph with overlapped I/O and CPU

Keep graph metadata, state, and allocation persistent; double-buffer weight tiles while CPU kernels consume the prior tile. This attacks setup and idle time and is likely a meaningful multiplier, but cannot overcome a fundamentally insufficient storage bandwidth or serial dependency chain.

### H7 — Output-directed / information-theoretic computation

Estimate whether the next-token decision can be certified from a small subset of weights or logits, using bounds, branch-and-bound, or adaptive computation. If a certificate is cheap for most steps, the full model need not be evaluated. This is the most radical exact-in-principle direction, but no general certificate is assumed; failure to certify must fall back to exact or explicitly approximate execution.

## 4. Plausibility of orders-of-magnitude improvement

The most plausible sources of 1,000×-class improvement are not a single faster matvec. They require changing the amount of model computation: H3 predictive verification, H4 a compact surrogate, or H7 adaptive certification. H1, H2, and H6 could plausibly deliver large practical factors by eliminating pathological traffic and startup overhead, but their ceiling is bounded by the work still required for exact inference. H5 may cross that boundary for models with exploitable structure, but must demonstrate stable compression and accuracy on real layers.

Near-instant exact equivalence remains an open question. QUORTEX will not claim it without a proof or exhaustive reference comparisons at the defined scope.

## 5. First three cheap, decisive experiments

### Experiment A — Establish the traffic floor

On a small representative layer and the real GGUF index, measure sequential versus random range-read bandwidth/latency, read amplification, CPU copy cost, and selected-expert bytes per token. Compare the measured lower bound with the 120-second baseline. Decision: if the minimum required bytes alone exceed the desired latency budget, reject storage-only explanations and prioritize computation-reducing hypotheses.

### Experiment B — Router/selection value and overlap

Replay recorded router IDs and tensor descriptors without running the full model. Compare (a) all-expert reads, (b) selected-expert reads, (c) coalesced selected reads, and (d) double-buffered reads overlapped with a synthetic quantized matvec. Decision: quantify the maximum possible gain from routing and overlap before spending time on full-model execution.

### Experiment C — Exactness ladder for a compact predictor

On a tiny layer/window with Cortex’s oracle, test a resident low-rank or state-space predictor and an uncertainty-triggered exact fallback. Measure hidden-state error, router-ID agreement, next-token agreement, fallback rate, TTFT, and subsequent-token time. Decision: determine whether prediction can reduce expensive evaluations while clearly labeling approximate output.

All three experiments are designed to use metadata, small tensor slices, recorded states, or synthetic kernels first. No large model download or full generation is justified until one hypothesis survives its decisive test.

## 6. Proposed QUORTEX execution architecture

QUORTEX should be a layered system with an immutable oracle boundary:

1. **Model index and transform compiler:** inspect model metadata, build a content-addressed tensor index, and optionally emit a QUORTEX-native tiled/fused representation. Never mutate the source GGUF.
2. **Cost and evidence planner:** estimate byte traffic, arithmetic, state size, and confidence for exact, approximate, speculative, or fallback execution.
3. **Persistent state service:** own recurrent state, KV/state caches, token history, and checkpoint identities independently of weight buffers.
4. **Router/certifier:** run shared and routing work first; expose selected tiles or an exactness certificate before expensive expert materialization.
5. **Asynchronous tile engine:** schedule aligned reads, decompression/dequantization, and CPU kernels through bounded double buffers with explicit lifetime fences.
6. **Execution backends:** exact streamed backend, approximate predictor backend, and speculative verifier backend. Each reports its semantic mode.
7. **Oracle and differential harness:** compare logits, hidden boundaries, state, routing IDs, token outputs, latency, RSS, read bytes, and read counts. The oracle is never altered to make a candidate pass.
8. **Benchmark ledger:** record hardware, kernel/build identity, model hash, prompt/token sequence, semantic mode, TTFT, per-token latency, peak RSS, storage traffic, and correctness result in machine-readable form.

The first implementation should be instrumentation and decisive experiments, not a large runtime rewrite. Source and tests will be imported from Cortex only after selecting their exact provenance and license; large weights, checkpoints, secrets, and generated binaries remain excluded.

## Open questions

- What is the exact model artifact and hash for the 90.78 GiB reference?
- Which portions of each token’s next-token decision are information-theoretically unavoidable?
- Can selected expert traffic be made sequential/coalesced enough to approach device bandwidth?
- Does a compact predictor maintain routing and token agreement beyond toy windows?
- What is the reference machine’s sustained storage bandwidth under the actual access pattern?

