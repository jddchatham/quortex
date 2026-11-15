# QIES macOS ARM64 (Apple Silicon) preparation record

Status: **bring-up phase complete — no model inference attempted.**
Date: 2026-11-15
Host: Mac mini M4 (arm64), 16 GB unified RAM, macOS 15.3.1 (24D70), Apple clang 17.0.0 (arm64-apple-darwin24.3.0), Python 3.9.6.
Branch: `dev/macos-arm64-prep` (local only; nothing pushed).

## What exists vs. what was missing

* No macOS/Darwin/ARM64 port existed: zero hits for darwin/macos/apple/arm64/aarch64
  across the tree; only remote branches are `main` (docs) and `quortex`;
  no stash, tags, unreachable objects, local clones, or preservation archives
  on this machine; the author's 22 public repositories contain no sibling port.
* The Linux guarded path (`quortex/autonomy/guarded_inference.py`) depends on
  `systemd-run --user` (`MemoryMax=2G MemorySwapMax=0 TasksMax=64`), hardcoded
  `/home/user/quortex` and `/home/user/bitnet-cortex/...` paths, pinned
  `/tmp/quortex-qies109-wt.Tgz8zp/...` builds, and XDG/DBUS user-manager
  endpoints.  `inference_service.validate` hardcodes a `/home/user/` prefix.

## Empirical Darwin findings (this host, verified by tests)

| Mechanism | Result |
|---|---|
| `setrlimit(RLIMIT_AS)` (lower only) | **rejected** ("current limit exceeds maximum limit") |
| `setrlimit(RLIMIT_RSS)` (lower only) | **rejected** (same) |
| `setrlimit(RLIMIT_STACK)` | rejected |
| `setrlimit(RLIMIT_NPROC/CORE/NOFILE/CPU/FSIZE)` | accepted |
| `RLIMIT_CPU` enforcement | **enforced** (child SIGXCPU'd, rc -24) |
| `getrusage(RUSAGE_SELF).ru_maxrss` in a trivial C++ binary | ~1.2 GB (shared-cache inflation, not workload memory) |
| `proc_pid_rusage(...).ri_phys_footprint` for the same binary | 0.2 MB (true workload footprint) |

Consequence: the Linux "RLIMIT_AS + systemd MemoryMax" pair has no direct
Darwin equivalent for user processes.  The supported replacement is a
supervisor-side monitor over `proc_pid_rusage` (libproc — same interface as
`top`/`ps`, same-user, no privileges) using **phys_footprint**, with
SIGKILL of the child's process group, plus the rlimits Darwin does accept
(NPROC=64 as the TasksMax analog, CORE=0, CPU=timeout+grace as backstop),
plus a parent-side wall-clock timeout.

## New macOS components (this branch only — production code untouched)

* `quortex/autonomy/macos_inference.py` — Darwin guarded launcher mirroring
  the Linux guard semantics fail-closed: fixed target allowlist (currently
  `arm64-smoke`), optional SHA-256 pins, fresh-empty output directory inside
  the repository, environment allowlist, single-instance `flock`, complete
  evidence JSON per attempt (limits, monitor metric, peak memory, flags).
  Nothing executes unless allowlisted; nothing model-related runs.
* `native/macos_arm64_smoke.cpp` + `native/build_macos_smoke.sh` — minimal
  arm64 Mach-O target: deterministic float64 dot product over LCG-generated
  vectors with an embedded reference and 1e-9 relative self-check
  (fail-closed exit status).  No CMake, no external source trees.
* `tests/test_macos_inference.py` — 8 bounded tests: guard rejections, Darwin
  rlimit application, sampler sanity, RSS-cap kill (rc 137), timeout kill
  (rc 124), well-behaved completion.
* `tests/test_macos_smoke.py` — 4 tests: Mach-O arm64 identity, independent
  Python recomputation of the reference (rel. < 1e-12), binary self-check,
  supervised run with no guard tripped.

## Numerical cross-verification (step 9 evidence)

* Reference (Python float64, independent implementation):
  `dot = -840.45440460774125` for N = 2^20 LCG pairs.
* Native arm64 binary (clang -O2): `SMOKE_DOT = -840.45440460774148`,
  `SMOKE_REL_ERR = 2.71e-16`, `SMOKE_PASS=1`, exit 0.
* Agreement across independent implementations within 3e-16 relative —
  numerical path sane for bounded synthetic workloads.

## Test results (actual)

* Existing suite, unmodified: **Ran 93 tests, FAILED (failures=1, errors=3)**
  = 89 pass.  The 4 non-passing tests all require the external Cortex source
  tree at hardcoded Linux paths (`/home/user/bitnet-cortex/...`,
  `/tmp/quortex-qies109-wt.Tgz8zp/...`) and fail with `FileNotFoundError` /
  missing-file assertions.  They are environment blockers, not code defects,
  and are expected to pass on a host with the Cortex checkout mounted.
* New macOS tests: **12/12 pass** (run repeatedly; no flakes after fixes).
* Fail-closed checks observed live: unapproved target → `GUARD_REJECTED` rc 2;
  RSS cap breach → SIGKILL rc 137 with `rss_cap_enforced: true`; timeout →
  rc 124 with `timeout_enforced: true`.

## Fixes applied during bring-up (each caught by tests)

1. RSS cap originally only flagged; now kills immediately (test duration
   30 s → ~2 s).
2. `RUsageInfoV0` ctypes struct was missing the two trailing abstime fields
   → kernel wrote past the buffer → segfault of the supervising process.
   Struct corrected; 12/12 stable since.
3. Memory metric switched `resident_size` → `phys_footprint` after measuring
   the Darwin shared-cache inflation of `ru_maxrss`.

## Memory-safety / verification invariants preserved

* Fail-closed everywhere: allowlist, hash pins, fresh-empty in-repo output,
  env allowlist, single-instance lock — no guard was disabled or weakened to
  pass any test.
* `quortex/autonomy/acceptance.py` untouched: `two_token_pass` still demands
  `status == PASS`, zero exits, proven state continuity, `numerical_comparison
  == PASS`, and non-empty captures for both reference and QUORTEX.
* No model file was loaded, no GGUF touched, no large-model run attempted.

## Remaining blockers for real inference on this Mac

1. **Cortex source tree**: `native/` targets and 4 static-analysis tests need
   `cortex-stream-forge` (cloneable from GitHub; CMake paths must be adapted
   from `/home/user/...` — via a macOS CMake toolchain/preset, not source
   edits).  ggml builds fine on Darwin normally; still needs verification.
2. **Model weights**: none of the FlashNext shards referenced by
   `flashnext-tensor-index.json` exist locally; no download attempted.
3. **Hash pins**: `APPROVED_SHA256` in the Darwin launcher is intentionally
   empty until a verified local build is pinned (mirrors the Linux pin
   discipline).
4. **Hardcoded Linux paths** in `guarded_inference.py` / `inference_service.py`
   would need an env-root or config-based root for any future native-service
   port (production change — not done here).

## Next safe milestone

M-MAC-01: clone `cortex-stream-forge` read-only, build **ggml-only** targets
(`quortex-gguf-diagnostic`, `quortex-tensor-allocation`) against it with
`-DGGML_NATIVE=ON` (host arm64), run both under `macos_inference.py` with the
existing guards, and only then evaluate the llama-linked FlashNext targets.
No model weights, no inference, until M-MAC-01 evidence is recorded.
