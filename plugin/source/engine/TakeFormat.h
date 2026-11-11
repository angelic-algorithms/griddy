#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <vector>

namespace griddy
{

/** Everything the engine needs to know about one take. Stored as take.json next to the media.

    A take is a folder named <takeId>.gtake:
        take.json            this struct
        audio.wav            the plugin's reference copy of the track audio (mono, 16 bit)
        frames/000000.jpg    every frame at full size, each one an intraframe
        proxy/000000.jpg     the same frames at tile size, for 9 slot playback
*/
struct TakeMeta
{
    enum class State { recording, review, kept, archived, discarded };

    int32_t takeId = -1;
    int slot = 0;
    State state = State::recording;

    double sampleRate = 48000.0;
    int64_t startHostSample = 0;      // host time of the first reference audio sample
    double startPpq = 0.0;
    double bpm = 120.0;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;

    double cameraLatencySeconds = 0.0;  // calibration: how long before its timestamp the scene happened
    double deliveryDelaySeconds = 0.0;  // measured: arrival time minus capture time, median (diagnostic)
    double audioLatencySeconds = 0.0;   // host reported input latency, if any
    double slipSeconds = 0.0;           // user adjustment, positive shows the video later

    double durationSeconds = 0.0;       // length of the reference audio
    int width = 1280, height = 720;
    int proxyWidth = 640, proxyHeight = 360;
    double fps = 30.0;
    int numFrames = 0;

    bool embeddedSync = false;          // audio.wav carries the Griddy sync signal
    int embeddedTag = 0;
    double createdAt = 0.0;             // seconds since the epoch

    /** Seconds from the first reference audio sample to each frame's scene time. */
    std::vector<double> frameTimes;

    /** Reference loudness every 0.1 s, for telling a trimmed clip from a quiet passage. */
    std::vector<float> envelope;

    /** Where on the video timeline the picture of frame i belongs, after latency and slip. */
    double videoOffsetSeconds() const noexcept { return audioLatencySeconds - cameraLatencySeconds + slipSeconds; }

    double hostStartSeconds() const noexcept { return (double) startHostSample / sampleRate; }

    juce::var toVar() const;
    static TakeMeta fromVar (const juce::var& v);

    static const char* stateName (State s);
    static State stateFromName (const juce::String& n);
};

bool writeTakeMeta (const juce::File& folder, const TakeMeta& meta);
bool readTakeMeta (const juce::File& folder, TakeMeta& meta);

/** Minimal mono 16 bit WAV so the core needs no audio format library. */
class WavWriter
{
public:
    bool open (const juce::File& file, double sampleRate);
    void write (const float* samples, int n);
    /** Fixes up the header and closes. Safe to call twice. */
    void close();
    int64_t numWritten() const noexcept { return count; }
    ~WavWriter() { close(); }

private:
    void writeHeader();
    std::unique_ptr<juce::FileOutputStream> out;
    double sr = 48000.0;
    int64_t count = 0;
};

/** Reads a whole mono WAV written by WavWriter. */
bool readWav (const juce::File& file, std::vector<float>& samples, double& sampleRate);

/** The RMS envelope used for TakeMeta::envelope. */
std::vector<float> computeEnvelope (const float* audio, size_t n, double sampleRate, double step = 0.1);

} // namespace griddy
