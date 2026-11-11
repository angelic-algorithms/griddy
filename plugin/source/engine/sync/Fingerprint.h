#pragma once

#include "../ClipSource.h"
#include "../SeqLock.h"
#include "../SpscRing.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace griddy::fingerprint
{

// Analysis runs at 8 kHz with a 512 point FFT and a hop of 64 samples (8 ms per frame).
inline constexpr int analysisRate = 8000;
inline constexpr int fftSize = 512;
inline constexpr int hop = 64;
inline constexpr double frameSeconds = (double) hop / analysisRate;

struct Landmark
{
    uint32_t hash;
    int32_t frame; // frame of the later peak of the pair
};

/** Streaming front end: any sample rate in, 8 kHz peaks and landmarks out. */
class Analyzer
{
public:
    explicit Analyzer (double inputRate, bool keepAllAudio = false);

    /** Feeds audio. New landmarks are appended to out. */
    void push (const float* audio, int numSamples, std::vector<Landmark>& out);

    /** The 8 kHz audio produced so far that is still needed for refinement (newest last). */
    const std::vector<float>& recent8k() const noexcept { return history8k; }
    int64_t samples8kProduced() const noexcept { return produced8k; }
    double inputPerSample8k() const noexcept { return ratio; }
    int64_t frameCount() const noexcept { return framesDone; }

private:
    struct Peak { int32_t frame; int16_t bin; };

    void processFrame (std::vector<Landmark>& out);

    double ratio; // input samples per 8 kHz sample
    bool keepAll;
    double nextOut = 0.0;
    int64_t inIndex = 0;
    double prevY = 0.0;
    struct Biquad { double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0; double process (double x) noexcept { const auto y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } } lp[2];

    std::vector<float> window, ring, history8k, mag;
    std::vector<double> re, im;
    int64_t produced8k = 0;
    int64_t framesDone = 0;
    std::deque<Peak> recentPeaks;
};

/** Offline: the landmarks of a whole take plus its 8 kHz audio, for matching and refinement. */
class Index
{
public:
    Index (const float* audio, size_t numSamples, double sampleRate);

    int takeId = -1;
    double durationSeconds = 0.0;
    std::vector<uint64_t> entries; // (hash << 32) | frame, sorted
    std::vector<int16_t> audio8k;  // for refinement

    /** All reference frames whose landmark has this hash. */
    std::pair<std::vector<uint64_t>::const_iterator, std::vector<uint64_t>::const_iterator> find (uint32_t hash) const;
};

struct Match
{
    int takeIndex = -1;
    double takeSeconds = 0.0;   // position in the take at the end of the newest live audio
    int votes = 0;
    float confidence = 0.0f;
};

/** Votes live landmarks against the reference indexes. */
class Matcher
{
public:
    Matcher() = default;

    void setIndexes (std::vector<std::shared_ptr<const Index>> idx);
    int numIndexes() const noexcept { return (int) indexes.size(); }
    const Index& index (int i) const { return *indexes[(size_t) i]; }

    void addLandmarks (const std::vector<Landmark>& lm);

    /** Drops votes older than the window. Call with the current live frame. */
    void expire (int32_t liveFrame);

    /** Best current match, or takeIndex -1. */
    Match best (int32_t liveFrame) const;

    /** Offset in frames (reference minus live) that won, valid when best().takeIndex >= 0. */
    int32_t bestOffset = 0;

private:
    std::vector<std::shared_ptr<const Index>> indexes;
    struct Vote { int32_t liveFrame; int32_t takeIndex; int32_t offset; };
    std::deque<Vote> votes;
    std::unordered_map<uint64_t, int> counts;

    static uint64_t key (int takeIndex, int32_t offset) { return ((uint64_t) (uint32_t) takeIndex << 32) | (uint32_t) offset; }
};

/** ClipSource built from the above. Takes are set from the message thread via setIndexes(). */
class FingerprintClipSource final : public griddy::ClipSource
{
public:
    explicit FingerprintClipSource (double sampleRate);

    SyncKind kind() const noexcept override { return SyncKind::fingerprint; }
    void feed (const float* mono, int numSamples, int64_t hostSample, bool playing) noexcept override;
    void process() override;
    ClipFix latest() const noexcept override { return fix.read(); }

    /** Replaces the takes to match against. Safe to call from another thread. */
    void setIndexes (std::vector<std::shared_ptr<const Index>> idx);

    /** True while audio is flowing but nothing matches (warped, effected, bounced or a different take). */
    bool noMatchWhileActive() const noexcept { return unmatched.load(); }

    /** Frames of audio analysed, for tests. */
    int64_t liveFrames() const noexcept { return framesAnalysed.load(); }

private:
    struct Stamp { int64_t streamStart, hostSample; int32_t n; int32_t pad; };

    void evaluate();

    double sr;
    SpscRing<float> audio;
    SpscRing<Stamp> stamps;
    int64_t streamWritten = 0;
    Analyzer analyzer;
    Matcher matcher;
    std::deque<Stamp> recent;
    std::vector<float> scratch;
    std::vector<Landmark> landmarks;
    SeqLock<ClipFix> fix;
    uint32_t serial = 0;
    int64_t streamConsumed = 0;
    int64_t framesSinceEval = 0;
    double rms = 0.0;
    std::atomic<bool> unmatched { false };
    std::atomic<int64_t> framesAnalysed { 0 };

    std::mutex indexLock;
    std::vector<std::shared_ptr<const Index>> pendingIndexes;
    std::atomic<bool> haveNewIndexes { false };

    double lastLockedActivity = 0.0;
    int64_t activeSamples = 0; // consecutive samples with audible level
};

} // namespace griddy::fingerprint
