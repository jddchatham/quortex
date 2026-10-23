# QIES requirements and traceability

QIES-001 is project guidance, not a certification claim.

| ID | Requirement | Implementation/evidence |
|---|---|---|
| Q-01 | Real GGUF loads and generates locally | `QUORTEX_RUNTIME_BASELINE.md`; direct Qwen2.5 run |
| Q-02 | Deterministic quadrant/officiation | `quortex/quadrant.py`; `tests/test_quadrant.py` |
| Q-03 | Gate occurs before backend load | `quortex/cli.py`; `tests/test_inference.py` |
| Q-04 | Direct-vs-wrapper transcript correctness | Open; Stage A next action |
| Q-05 | Large FlashNext execution | Open; `docs/QIES_LARGE_MODEL_STATUS.md` |
| Q-06 | Measured inference-path optimization | Open; synthetic gate savings do not satisfy this |
| Q-07 | Reproducible records and recovery | `quortex/autonomy/state.json`; `docs/QIES_RECOVERY.md` |
| Q-08 | No false autonomy claim | `docs/QIES_AUTONOMY_STATUS.md` |

