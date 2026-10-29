#include "flashnext-layer-driver.h"
#include <cstdio>
 #include <vector>
#include <fstream>
#include <string>
#include <sys/stat.h>
static bool capture(const char * dir, int layer, const std::vector<float> & v) {
    std::string p = std::string(dir) + "/quortex_layer" + std::to_string(layer) + "_p0.bin";
    std::string t = p + ".tmp"; std::ofstream f(t, std::ios::binary);
    if (!f.write(reinterpret_cast<const char *>(v.data()), v.size()*sizeof(float))) return false;
    f.close(); return std::rename(t.c_str(), p.c_str()) == 0;
}
int main(int argc, char ** argv) {
    if (argc != 4) { std::fprintf(stderr, "usage: %s MODEL.gguf tensor-index.json OUTPUT_DIR\n", argv[0]); return 2; }
    flashnext_layer_driver * d = flashnext_layer_driver_open(argv[1], argv[2], 0, 2);
    if (!d) { std::puts("CHAIN_OPEN=FAIL"); return 3; }
    std::vector<float> input(2560), output(10240);
    if (flashnext_layer_driver_embedding(d, 248045, input.data(), input.size()) != 2560) {
        flashnext_layer_driver_close(d); std::puts("CHAIN_EMBEDDING=FAIL"); return 4;
    }
    const int n0 = flashnext_layer_driver_run_layer(d, 0, 0, input.data(), input.size(), output.data(), output.size());
    if (n0 != 10240 || !capture(argv[3], 0, output)) { flashnext_layer_driver_close(d); return 5; }
    input.assign(output.begin(), output.end());
    const int n1 = flashnext_layer_driver_run_layer(d, 1, 0, input.data(), input.size(), output.data(), output.size());
    if (n1 != 10240 || !capture(argv[3], 1, output)) { flashnext_layer_driver_close(d); return 6; }
    std::printf("CHAIN_RANGE=[0,2) LAYER0=%d LAYER1=%d STATE_BYTES=%zu\n", n0, n1, flashnext_layer_driver_state_size(d));
    flashnext_layer_driver_close(d);
    return n1 == 10240 ? 0 : 4;
}
