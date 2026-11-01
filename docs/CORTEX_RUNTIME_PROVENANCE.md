# Auditable Cortex staged runtime

The QUORTEX staged artifact was built from the separate
`cortex-stream-forge` repository at commit
`dea54af133e4b71f159ea57b7d172c4c6b452ecf`.

The local source checkout used for the verified build was:

```text
/tmp/quortex-qies109-wt.Tgz8zp/runtime-flashnext-layer-exp
```

That path is host-specific and is not required to be the checkout path for a
rebuild. The runtime source must be obtained from the identified repository and
commit. The Cortex source is intentionally not copied into this repository.

## Verified artifacts

```text
llama-flashnext-two-range
SHA256 13215449904d87d04b7fc48fc6ffa214878594d26622bb89898019c668af8029

libllama.so.0.5.0
SHA256 53df80306688a700aa02f5370c131b8e9064a1e139f7a7ad826463dd660de934

libggml-base.so.0.25.3
SHA256 8ea7ef14ba0355b870ae7c8be2b760507203071e50ec67a2239ee65ce58479a1
```

The build used CMake `Release`, `/usr/bin/cc`, `/usr/bin/c++`, CPU GGML
backend support, and disabled GGML tests/examples for the dedicated runtime.
Runtime linkage must resolve to the libraries beside the executable.

Relevant source changes at the Cortex revision include:

- Qwen4Exp staged Stage-A graph construction and recurrent execution.
- Omission of unused Stage-A KV inputs (`2cdd93b`).
- Post-allocation input audit instrumentation (`16fdcea`).
- Safe failure before binding unbacked required inputs (`dea54af`).
- Router and `flashnext_segment_a_boundary` capture.
- Staged MoE Stage-B/Stage-C integration seams, whose complete numerical
  correctness remains unproven.

## Verified status

Real-model Stage-A graph computation, synchronization, router capture, and
boundary capture have been observed. The Stage-A boundary was 12,804 F32
values and router output was 512 F32 values.

Still unproven: complete Stage-B and Stage-C execution, all 48-layer numerical
equivalence, final vocabulary logits, token generation, and multi-turn state
continuity.

The deployment uses host-specific model/index paths and the existing bounded
systemd launcher. No model files, generated binaries, or private diagnostic
evidence belong in this repository.
