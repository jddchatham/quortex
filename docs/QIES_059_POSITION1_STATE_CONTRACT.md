# QIES-059 — Position-1 State Contract

Status: investigation complete; no position-1 model execution performed.

## Required continuation

Position 1 must run in the same logical `llama_context` state lineage as
position 0, or after an exact state serialization/restoration into an equivalent
context. A fresh position-0 context is not a valid continuation.

The continuation record is:

```text
model identity and tensor-index identity
token sequence: token_0, token_1
sequence id: 0
position: 0 -> 1
initial state: zero/empty state defined by context creation
state after position 0: exact serialized sequence state
state before position 1: byte-identical restored state
```

## State components

The Cortex runtime owns state inside `llama_context` and its hybrid memory
context. The relevant components are:

1. Recurrent DeltaNet/SSM state for recurrent layers, including layer-specific
   convolution history and recurrent matrices.
2. Hyper-Connection residual streams, which are computed activations and must
   be passed from the preceding position/layer contract rather than recreated
   from a zero state.
3. Attention KV state for full-attention layers, including sequence position
   and cache slot ownership.
4. Sequence metadata: sequence id, token position, output selection, and the
   token identity used to build the next graph.

The runtime exposes whole-sequence serialization through:

- `llama_state_seq_get_size(ctx, 0)`
- `llama_state_seq_get_data(ctx, data, size, 0)`
- `llama_state_seq_set_data(ctx, data, size, 0)`

It also exposes diagnostic recurrent state through
`debug_copy_recurrent_state(layer, convolution, ...)`. The latter is useful
for component-level comparison but is not a replacement for the complete
sequence state because it does not by itself represent attention KV state or
all scheduler metadata.

## Position-1 acceptance protocol

Before execution, capture the serialized state after position 0 and restore it
into a separately initialized equivalent context. Acceptance requires:

- restoration succeeds with the exact serialized byte count;
- restored state hash equals the position-0 post-state hash;
- token 1 and position 1 are explicit and deterministic;
- no state component is silently reinitialized;
- position-1 intermediate captures agree with the trusted reference;
- position-1 output and post-state agree within justified tolerances.

The first test should use a fixed two-token input and compare, at minimum:

- position-0 state bytes/hash;
- position-1 Segment-A boundary;
- position-1 router logits;
- selected/shared/combined MoE;
- post-Segment-B output;
- position-1 state bytes/hash.

## Current blocker

QIES-056 proves only one position-0 token. The current QUORTEX full-layer
adapter exits after the isolated position-0 Segment-B result and does not yet
retain a reusable llama context or expose a position-1 input/state contract.
The next implementation must add state capture/restore and a two-position
diagnostic mode before any guarded position-1 execution is authorized.
