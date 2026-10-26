# Large-model status

The local Cortex records identify a Qwen FlashNext GGUF set under `/home/user/models/qwen38-flashnext-abliterated` and an Ollama Qwen3.8 blob at `/home/user/.ollama/models/blobs/sha256-6c2c13cef89238c3604d756b07b3ef5fafebbd61095feb8553ff449c95e4c1c6`. The large files are external assets and are not copied into Git.

## 2026-10-26 — M3-PREP evidence

The three target shards are present and total `97,473,155,200` bytes (`90.78
GiB`): `00001` 44,766,155,936 bytes, `00002` 44,735,995,008 bytes, and
`00003` 7,971,004,256 bytes. The existing Cortex ledger
`/home/user/bitnet-cortex/cortex-stream-forge/flashnext-sha256.txt` records
SHA-256 values for all three shards plus the separate MTP draft, LoRA, and
mmproj artifacts. No files were copied or modified.

The Cortex integration seam is identified at the frozen reference commit
`8da466d`: `runtime-flashnext-layer-exp/src/llama-model.cpp:1238-1246` keeps
separate global/layer buffers and a tensor-offset index; the owned-layer
experiment uses bounded `pread` reads in
`runtime-flashnext-layer-exp/examples/flashnext-owned-layer.cpp:145-153`.
The provider test also validates descriptor-offset bytes and buffer reuse
without mapping the complete model (`tests/test_weight_provider.py:21-48`).
This is metadata/provider evidence only; full generation and model-backed
equivalence remain unaccepted.

The Cortex records establish low-residency and isolated transaction evidence, but QUORTEX has not independently completed coherent multi-token large-model generation, logits validation, or full acceptance. The ~97.5 GiB FlashNext set is specifically not claimed as working here.
