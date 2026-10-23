# QUORTEX

Experimental research project for radically faster inference of enormous language models on memory-constrained computers.

QUORTEX is a separate research line from [Cortex Stream Forge](https://github.com/jddchatham/cortex-stream-forge). Cortex remains the known-correct reference implementation and is not modified by this repository.

Start with [QUORTEX_GENESIS.md](QUORTEX_GENESIS.md). It records the inherited evidence, open limits, architectural hypotheses, cheap falsification experiments, and the proposed execution architecture.

## Local inference experiment

The launcher gates a trusted llama.cpp-compatible backend before model loading:

```bash
PYTHONPATH=. python3 -m quortex.cli \
  -m /home/user/models/qwen/Qwen2.5-Coder-3B-Instruct-abliterated-Q4_K_M.gguf \
  --backend /home/user/bitnet-cortex/phase8-build/bin/llama-cli \
  --require model --require runtime --require chat
```

Add `--json-decision` to inspect the contextual decision without launching the backend. A rejected context exits before any GGUF load. Once admitted, QUORTEX delegates mathematical execution to the unchanged trusted runtime; it does not claim that admission makes the model output correct.

## Research discipline

- Claims are tagged as measured, derived, hypothesized, or unknown.
- Exact equivalence, numerical approximation, and behavioral similarity are separate outcomes.
- Every optimization retains an independently testable oracle.
- Models, checkpoints, secrets, and generated binaries stay outside version control.
