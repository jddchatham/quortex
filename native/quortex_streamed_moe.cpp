#include "quortex_streamed_moe.h"
#include "flashnext-selected-moe.h"
#include "flashnext-shared-expert.h"
#include "flashnext-tensor-index.h"
#include "flashnext-layer-transaction.h"

#include <algorithm>
#include <cmath>
#include <fcntl.h>
#include <unistd.h>

bool quortex_compute_streamed_moe(const char * path, const float * pre_moe,
        const float * router, quortex_moe_result & out, size_t arena_bytes) {
    if (!path || !pre_moe || !router) return false;
    flashnext_tensor_index index; std::string error;
    if (!index.load(path, &error)) return false;
    flashnext_layer_transaction tx; tx.layer = 0;
    std::copy(pre_moe, pre_moe + tx.hidden, tx.pre_moe_hidden.begin());
    std::array<int, 512> order{}; for (int i = 0; i < 512; ++i) order[i] = i;
    std::partial_sort(order.begin(), order.begin() + tx.experts_used, order.end(),
        [router](int a, int b) { return router[a] > router[b]; });
    double z = 0.0; double e[tx.experts_used]; const double mx = router[order[0]];
    for (size_t i = 0; i < tx.experts_used; ++i) { e[i] = std::exp(router[order[i]] - mx); z += e[i]; }
    for (size_t i = 0; i < tx.experts_used; ++i) {
        tx.selected_expert_ids[i] = order[i]; tx.routing_weights[i] = e[i] / z;
    }
    if (!flashnext_execute_selected_moe(index, path, tx, arena_bytes)) return false;
    flashnext_tensor_descriptor sg, su, sd, sc;
    if (!index.resolve_shared(0, flashnext_projection::gate, sg, &error) ||
        !index.resolve_shared(0, flashnext_projection::up, su, &error) ||
        !index.resolve_shared(0, flashnext_projection::down, sd, &error) ||
        !index.resolve_shared(0, flashnext_projection::shared_control, sc, &error)) return false;
    int fd = ::open(sg.path.c_str(), O_RDONLY | O_CLOEXEC); if (fd < 0) return false;
    flashnext_shared_expert_slices shared{{fd, fd, fd, fd}, sg.offset, su.offset, sd.offset, sc.offset};
    const bool ok = flashnext_execute_shared_expert(shared, tx); ::close(fd); if (!ok) return false;
    for (size_t i = 0; i < tx.hidden; ++i) tx.complete_moe[i] = tx.selected_moe[i] + tx.shared_moe[i];
    out.selected.assign(tx.selected_moe.begin(), tx.selected_moe.end());
    out.shared.assign(tx.shared_moe.begin(), tx.shared_moe.end());
    out.complete.assign(tx.complete_moe.begin(), tx.complete_moe.end());
    out.expert_ids.assign(tx.selected_expert_ids.begin(), tx.selected_expert_ids.end());
    out.routing_weights.assign(tx.routing_weights.begin(), tx.routing_weights.end());
    return std::all_of(out.complete.begin(), out.complete.end(), [](float v) { return std::isfinite(v); });
}
