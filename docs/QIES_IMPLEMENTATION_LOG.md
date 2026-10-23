# Implementation log

## db9862e

Added the trusted-backend launcher, pre-load contextual gate, JSON decision mode, and tests. A review found and fixed an initial bug where JSON inspection still launched the backend.

## Current limitation

The launcher delegates conversation formatting to the existing llama.cpp runtime. The first integrated interactive run showed unexpected transcript behavior; no multi-turn correctness claim is accepted until a deterministic harness passes.

## 2026-10-22 — Persistent session harness bounded

Added `benchmarks/persistent_session.py`, which keeps one backend process alive,
sends a fixed turn sequence through a PTY, and records command, requested/sent
turns, exit code, timeout, elapsed time, raw combined output, and SHA-256. Its
protocol tests use a deterministic two-turn fake backend.

The real Qwen2.5/Cortex `llama-cli` probe was bounded to 8 seconds and sent both
turns, but exited with code 255 and `input is empty` before producing a reply.
This is a backend interaction blocker; no persistent transcript equivalence or
state-reuse claim is accepted.

