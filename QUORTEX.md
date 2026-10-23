# QUORTEX operating brief

QUORTEX investigates contextual convergence for memory-constrained inference.

The quadrant state is ordered as `0/0`, `1/0`, `0/1`, and `1/1`: neither component, first component, second component, or both components. `1/1` means a candidate is complete, not that it is correct. A complete candidate must be officiated against the current requirements. Requirements can change, so an accepted decision is always tied to a context identity and must be reevaluated when that identity changes.

The initial prototype is deliberately model-independent. It must demonstrate deterministic state transitions, contextual rejection, invalidation on changed requirements, and a pre-inference gate whose avoided work is measured. It must not alter Cortex mathematics or claim a speedup for the quadrant mechanism without a benchmark.

The Cortex integration seam is a planner before expensive segment execution: metadata and context may reject or defer a candidate before weight reads or graph execution. Any exact model result remains subject to Cortex’s independent oracle and numerical regression.

