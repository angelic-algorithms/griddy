#pragma once

#include "../ClipSource.h"
#include "../SpscRing.h"
#include "../SeqLock.h"

#include <cstdint>
#include <deque>
#include <vector>

namespace griddy::embedded
{

// The Griddy sync signal: a faint 18 kHz carrier, differential BPSK, 800 baud.
// One symbol is round(sampleRate / 400) samples. A frame is 48 bits (16 sync, 8 take tag,
// 16 frame counter, 8 CRC) so it lasts 120 ms at 48 kHz. Frame k of a take covers take
// samples ((48 k + 1) L) to ((48 k + 49) L), after one reference symbol at the start.
inline constexpr double carrierHz = 18000.0;
inline constexpr int baud = 800;
inline constexpr int frameBits = 48;
inline constexpr uint16_t syncWord = 0xB5C3;
inline constexpr float defaultAmplitude = 0.003f; // about minus 50 dBFS

inline int symbolSamples (double sampleRate) { return (int) (sampleRate / baud + 0.5); }
uint8_t crc8 (uint8_t tag, uint16_t counter);

/** Adds the sync signal to a take's audio while it is being recorded. */
class Encoder
{
public:
    Encoder (double sampleRate, uint8_t takeTag, float amplitude = defaultAmplitude);

    /** Adds the signal to audio in place and advances. Call with consecutive blocks of the take. */
    void mixInto (float* audio, int numSamples);

    int64_t position() const noexcept { return sampleIndex; }

private:
    bool bitForSymbol (int64_t symbol) const;

    double sr;
    int symLen;
    uint8_t tag;
    float amp;
    int64_t sampleIndex = 0;
    int64_t currentSymbol = -1;
    double symbolPhase = 0.0; // accumulated differential phase in radians
};

struct Detection
{
    int64_t endIndex = 0;       // stream index of the first sample after the frame
    uint8_t takeTag = 0;
    uint16_t counter = 0;
    int64_t takeSample = 0;     // take sample that endIndex corresponds to
    float quality = 0.0f;       // mean carrier magnitude relative to the nominal amplitude
};

/** Finds sync frames in a stream. Cheap enough to run on every slot all the time. */
class Decoder
{
public:
    explicit Decoder (double sampleRate);

    /** Pushes samples and appends any frames that completed to out. */
    void push (const float* audio, int numSamples, std::vector<Detection>& out);

    /** Stream index of the next sample that push() will see. */
    int64_t streamPosition() const noexcept { return n; }

    void reset();

private:
    struct C { double re = 0.0, im = 0.0; };
    struct Biquad
    {
        double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double process (double x) noexcept
        {
            const auto y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    Biquad hp[2]; // 4th order high pass in front of the demodulator

    int symLen;
    double sr;
    int64_t n = 0;

    // local oscillator
    double oscRe = 1.0, oscIm = 0.0, rotRe = 1.0, rotIm = 0.0;

    // z history for the sliding sum, and per sample outputs
    std::vector<C> zHist;
    C sum;
    std::vector<float> dHist, magHist;
    std::vector<C> yHist;
    size_t histSize = 0;

    // candidate selection
    bool haveCandidate = false;
    int64_t candidateFirst = 0;
    Detection best;
    float bestScore = 0.0f;
    int64_t lastReportedEnd = -1;

    bool testAt (int64_t index, Detection& d, float& score) const;
    float dAt (int64_t i) const { return dHist[(size_t) (i % (int64_t) histSize)]; }
    float magAt (int64_t i) const { return magHist[(size_t) (i % (int64_t) histSize)]; }
};

/** ClipSource that turns decoder detections into fixes. */
class EmbeddedSyncClipSource final : public griddy::ClipSource
{
public:
    explicit EmbeddedSyncClipSource (double sampleRate);

    SyncKind kind() const noexcept override { return SyncKind::embedded; }
    void feed (const float* mono, int numSamples, int64_t hostSample, bool playing) noexcept override;
    void process() override;
    ClipFix latest() const noexcept override { return fix.read(); }

    /** Frames decoded since construction, for status and tests. */
    uint64_t numFramesDecoded() const noexcept { return frames.load(); }

private:
    struct Stamp { int64_t streamStart, hostSample; int32_t n; int32_t pad; };

    double sr;
    SpscRing<float> audio;
    SpscRing<Stamp> stamps;
    int64_t streamWritten = 0; // audio thread only
    Decoder decoder;
    std::deque<Stamp> recent; // worker only
    std::vector<float> scratch;
    std::vector<Detection> found;
    SeqLock<ClipFix> fix;
    uint32_t serial = 0;
    std::atomic<uint64_t> frames { 0 };
};

} // namespace griddy::embedded
