#include "flashnext-layer-driver.h"
#include <cstdio>
 #include <vector>
int main(int argc, char ** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: %s MODEL.gguf tensor-index.json\n", argv[0]); return 2; }
    flashnext_layer_driver * d = flashnext_layer_driver_open(argv[1], argv[2], 0, 2);
    if (!d) { std::puts("CHAIN_OPEN=FAIL"); return 3; }
    std::vector<float> input(2560, 0.0f), output(10240);
    const int n = flashnext_layer_driver_run(d, 0, input.data(), input.size(), output.data(), output.size());
    std::printf("CHAIN_RANGE=[0,2) OUTPUT_ELEMENTS=%d STATE_BYTES=%zu\n", n, flashnext_layer_driver_state_size(d));
    flashnext_layer_driver_close(d);
    return n == 10240 ? 0 : 4;
}
