# Research log

## Hypothesis R-01 — Context can prevent unnecessary full inference

- Saving: potentially 100% of backend work for candidates rejected before model load.
- Assumption: candidate metadata and requirements are known before inference.
- Risk: this is admission avoidance, not a speedup for accepted answers; stale context can reject valid work.
- Experiment: run identical accepted/rejected plans and verify backend process/load markers.
- Rejection criterion: any rejected plan starts the backend, or accepted outputs differ from the trusted oracle without an explicit approximation mode.
- Current result: rejected pre-load behavior passes; accepted-path computation savings are unproven.

## Hypothesis R-02 — Deterministic transcript/session reuse can reduce repeated prompt work

- Saving: prompt evaluation and model-load cost across turns.
- Assumption: backend state can be kept alive and transcript boundaries are stable.
- Risk: wrapper may alter chat templates or duplicate history.
- Experiment: compare a two-turn persistent session against two fresh direct invocations using tokenized transcripts and outputs.
- Status: harness implemented; real backend probe rejected because the available
  Cortex-era interactive mode exits with `input is empty` under the bounded PTY
  protocol. No inference-path benefit measured.

## 2026-10-26 — M1-DIAG result

The minimal protocol fault was the unspecified initial input boundary: `-cnv`
was used without `--interactive-first`. The harness now requests input-first
behavior. This repairs command semantics but does not establish transcript
equivalence or an optimization.
