# QIES architecture

The current architecture has three explicit layers:

1. **Trusted backend:** unchanged Cortex/llama.cpp executable performs GGUF loading, graph execution, sampling, and conversation state.
2. **QUORTEX admission plane:** `Officiator` evaluates a complete candidate against immutable contextual requirements before backend launch. A rejected candidate causes no model load.
3. **Future execution plane:** planned integration point for Cortex segment planning, exact state reuse, speculative verification, and selective weight scheduling. It is not implemented yet.

Completeness (`1/1`) is never treated as output correctness. Numerical and token correctness remain responsibilities of the trusted oracle and verification harness.

