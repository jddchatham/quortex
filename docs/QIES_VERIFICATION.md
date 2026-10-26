# Verification record

Current test command:

```bash
python3 -m unittest discover -s tests -v
```

Current result: 16 tests passed on 2026-10-22.

Verified:

- all four quadrant states;
- complete-but-contextually-invalid candidates;
- accepted contextual candidates;
- invalidation after requirement changes;
- rejected incomplete convergence;
- deterministic pre-inference admission;
- rejected backend launch;
- accepted plan construction;
- `--json-decision` does not launch a side-effecting backend.

Not yet verified: multi-turn semantic equivalence, large-model full generation, or inference-path speedup.

Single-turn oracle comparison passed: `benchmarks/compare_direct_wrapper.py` returned identical stdout and SHA-256 hashes for direct and QUORTEX execution with fixed seed `123`.

Persistent harness verification: `tests/test_persistent_session.py` proves one
backend process receives both turns and records a stable output hash, and that
an early backend exit is reported as incomplete. A real bounded probe using the
Qwen2.5 model and `/home/user/bitnet-cortex/phase8-build/bin/llama-cli` sent
both turns and loaded the GGUF, but bounded CPU probes did not complete prompt
evaluation/generation; multi-turn equivalence remains open.

M1-DIAG verification: focused persistent-session tests pass 35/35 and the full
suite passes 55/55 after adding `--interactive-first`; `llama-cli --help`
confirms the option is supported. No model-backed comparison was claimed.
