# Recovery procedure

1. `cd /home/user/quortex`
2. `git status --short --branch`
3. Read `quortex/autonomy/state.json` and `docs/QIES_NEXT_ACTION.md`.
4. Run `python3 -m unittest discover -s tests -v`.
5. Inspect `git log --oneline --decorate -5` and `git ls-remote --heads origin quortex`.
6. Continue only from the recorded state; never force-push.

Model weights, checkpoints, and generated binaries are external and must remain outside Git.

