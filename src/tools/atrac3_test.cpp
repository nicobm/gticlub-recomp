// atrac3_test file.msa out.f32 : decodes a whole .msa (MSF, ATRAC3) with recomp/atrac3 into planar float32
// (all of channel 0, then channel 1, ...) and prints the decoding time.
#include "recomp/atrac3.h"
#include <chrono>
#include <cstdio>
#include <vector>
#include <cstdlib>
namespace gti::recomp::atrac3 { extern bool g_debug; }

int main(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: atrac3_test file.msa out.f32\n"); return 1; }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::printf("cannot open %s\n", argv[1]); return 1; }
    std::vector<uint8_t> d;
    for (int c; (c = std::fgetc(f)) != EOF;) d.push_back(uint8_t(c));
    std::fclose(f);
    auto be32 = [&](size_t o) { return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3]; };
    const int codec = int(be32(4)), channels = int(be32(8));
    const size_t size = be32(12);
    if (codec != 5 || channels < 1 || channels > 8) { std::printf("not ATRAC3 105 kbps (codec %d, %d channels)\n", codec, channels); return 1; }
    const size_t block = size_t(0x98) * channels, frames = size / block;
    gti::recomp::atrac3::Decoder dec(channels, 0x98);
    std::vector<std::vector<float>> pcm(channels, std::vector<float>(frames * 1024));
    std::vector<float*> out(channels);
    int bad = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < channels; ++c) out[c] = pcm[c].data() + i * 1024;
        gti::recomp::atrac3::g_debug = std::getenv("A3DEBUG") && i < size_t(std::atoi(std::getenv("A3DEBUG")));
        if (!dec.decode(d.data() + 0x40 + i * block, out.data())) ++bad;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (FILE* o = std::fopen(argv[2], "wb")) { for (auto& p : pcm) std::fwrite(p.data(), 4, p.size(), o); std::fclose(o); }
    std::printf("%zu frames, %d channels, %d bad, %.1f ms (%.1f us per frame)\n", frames, channels, bad, ms, 1000.0 * ms / double(frames));
    return 0;
}
