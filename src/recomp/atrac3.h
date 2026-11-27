// ATRAC3 decoder (Sony's audio codec used by the game's .msa music and ambience streams), so the game no longer needs
// the decoder module of the PS3 system software (flashATRAC.pic).
//
// The algorithm and tables follow FFmpeg's libavcodec/atrac3.c and atrac.c (Copyright (c) 2006-2008 Maxim Poliakovski,
// Benjamin Larsson and others), licensed under the GNU Lesser General Public License 2.1 or later; this file and
// atrac3.cpp are distributed under the same licence. Only the non-joint-stereo mode the game's streams use is
// implemented: every channel is an independent sound unit of `unitBytes` bytes.
#pragma once
#include <cstdint>

namespace gti::recomp::atrac3 {

struct Channel;   // per-channel decoder state (overlap, gain control, QMF delay lines)

class Decoder {
public:
    explicit Decoder(int channels = 2, int unitBytes = 0x98);
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    void reset();
    // decodes one frame (channels * unitBytes bytes) into out[ch][1024], samples in about [-1, 1];
    // returns false (and outputs silence) when a sound unit is not valid
    bool decode(const uint8_t* frame, float* const* out);
    int channels() const { return nch; }
    // bits of channel c's sound unit read by the last decode (the module reports those of the last channel)
    int bitsUsed(int c) const { return c >= 0 && c < nch && c < 8 ? used[c] : 0; }
private:
    int nch, unit;
    int used[8] = {};
    Channel* ch;
};

}   // namespace gti::recomp::atrac3
