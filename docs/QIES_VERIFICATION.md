# Verification record

Current test command:

```bash
python3 -m unittest discover -s tests -v
```

Current result: 9 tests passed on 2026-10-22.

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

Not yet verified: wrapper/direct token equivalence, multi-turn semantic equivalence, large-model full generation, or inference-path speedup.

