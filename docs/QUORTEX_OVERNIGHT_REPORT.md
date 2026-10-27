# QUORTEX Overnight Engineering Report

## Starting baseline

QIES-056 was preserved at QUORTEX commit
`6779df8cdd1a2930a9158df96a76777716419498`.
The QIES-056 position-0 integrated pipeline was bitwise identical at all six
checkpoints, with 287.2 MiB peak QUORTEX cgroup memory.

## Work completed in this session

- Audited the separate Cortex checkout at revision
  `8da466d955635180190945412d5560403e4ff062`.
- Confirmed the Cortex checkout is dirty with many unrelated experimental
  changes and was not reset, cleaned, committed, or pushed.
- Identified the QIES-056-required Cortex files and preserved them as an
  external dependency inventory rather than mixing the checkout into QUORTEX.
- Created local development branch `qies-059-position1`.
- Added `docs/QIES_059_POSITION1_STATE_CONTRACT.md`.
- Added opt-in `QUORTEX_TWO_TOKEN_STATE=1` mode. It evaluates two
  embedding-backed tokens sequentially in one context, captures each output,
  serializes sequence state after each position, and rejects conflicts with
  the layer-0 diagnostic modes.
- Added a matching Cortex-side `CORTEX_REFERENCE_TWO_TOKEN_ONLY=1` diagnostic
  in the separate checkout. It uses the same context and token sequence and
  captures position-1 output and both serialized states; the Cortex checkout
  remains uncommitted because it contains unrelated dirty work.
- Added this report.

## Validation

The existing QUORTEX lightweight suite passed: `59 tests`.
Python compile checks passed and `git diff --check` passed.
No model execution was performed during this continuation session.
The two-token modes have been built and statically checked, but their numerical
continuation behavior remains unverified.

## Cortex dependency status

The QIES-056 reference/runtime changes remain in the separate dirty Cortex
checkout. Relevant changes include the FlashNext reference executable,
token-embedding row API, diagnostic capture APIs, Segment-B contract exports,
and Qwen4exp graph diagnostics. Unrelated Cortex changes and generated build
trees remain excluded. A clean isolated Cortex commit cannot be safely created
without separating the existing dirty work first.

## Position-1 status

Position 1 is implemented as matching opt-in sequential-context diagnostic
modes but is not numerically verified. The state contract identifies whole sequence
state, recurrent state, attention KV state, hyper-connection streams, sequence
metadata, and exact serialization/restoration requirements.

No position-1 execution was attempted, so there are no new runtime or memory
measurements.

## Resume commands

```sh
cd /home/user/quortex
git status --short
git branch --show-current
python3 -m unittest discover -s tests -p 'test*.py' -q
python3 -m compileall -q tools tests
git diff --check
```

Before a position-1 run, implement and statically validate exact state capture,
restore, corruption rejection, and a two-position diagnostic mode. Then use
one guarded reference run and one guarded QUORTEX run with the established
2-GiB/zero-swap/64-task/1536-MiB-address-space limits.

## Remaining risks

- Cortex dependency changes are not independently committed.
- Position-1 state ownership and restoration are not yet proven.
- QIES-056 covers one token and position 0 only.
- Full 48-layer and multi-turn correctness remain unverified.
