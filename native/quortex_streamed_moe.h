#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct quortex_moe_result {
    std::vector<float> selected;
    std::vector<float> shared;
    std::vector<float> complete;
    std::vector<int32_t> expert_ids;
    std::vector<double> routing_weights;
};

// Computes layer-0 MoE output from QUORTEX-owned Segment-A values and the
// version-matched streamed Cortex kernels. No reference output is consumed.
bool quortex_compute_streamed_moe(const char * tensor_index_path,
        const float * pre_moe, const float * router, quortex_moe_result & result,
        size_t arena_bytes = 16 * 1024);
