#pragma once

#include "SpscRing.h"

#include <juce_graphics/juce_graphics.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace griddy
{

struct ExportSettings
{
    juce::File file;
    int width = 1280, height = 720;   // rounded down to even numbers
    int fps = 30;
    double sampleRate = 48000.0;
};

/** What a finished file contains, read back. Used by tests and by the status line. */
struct MediaInfo
{
    bool ok = false;
    int width = 0, height = 0;
    int videoFrames = 0;       // 0 when the platform cannot count cheaply
    double videoSeconds = 0.0;
    double audioSeconds = 0.0;
    double fileSeconds = 0.0;
    int audioChannels = 0;
    juce::String videoCodec, audioCodec, problem;
};

/** An encoder for one MP4 file. Used from the writer thread only. */
class ExportBackend
{
public:
    virtual ~ExportBackend() = default;
    virtual bool open (const ExportSettings& s, juce::String& error) = 0;

    /** bgra: width * height pixels, rows `stride` bytes apart. pts in seconds from the start of the file. */
    virtual bool writeVideo (const uint8_t* bgra, int stride, double pts) = 0;

    /** Interleaved stereo 16 bit; firstFrame counts sample frames from the start of the file. */
    virtual bool writeAudio (const int16_t* stereo, int frames, int64_t firstFrame) = 0;

    virtual bool close (juce::String& error) = 0;
};

/** Backend for this platform: FFmpeg (H.264 and AAC) where the build has it, AVFoundation on a Mac. nullptr if neither. */
std::unique_ptr<ExportBackend> createExportBackend();
bool gridExportAvailable();

/** Opens a finished file and reports what is in it. */
MediaInfo probeMedia (const juce::File& file);

/** Records the grid and the mix to an MP4 while the DAW plays.

    The audio thread calls pushAudio() (wait free, into a ring); the control thread calls pushVideo() with the composited
    grid. A writer thread owns the encoder, so neither caller ever waits on it. Picture time stamps are the audio sample count
    at the moment the picture was drawn, so picture and sound share one clock.
*/
class GridExporter final : private juce::Thread
{
public:
    GridExporter();
    ~GridExporter() override;

    bool start (const ExportSettings& settings, juce::String& error);
    bool isRunning() const noexcept { return running.load(); }

    /** Audio thread. Stereo (or mono, copied to both sides) float samples. */
    void pushAudio (const float* left, const float* right, int numSamples) noexcept;

    /** Seconds of audio pushed so far: the export clock. */
    double clockSeconds() const noexcept;

    /** Any thread but the audio thread. Converts and queues one picture. Dropped if the writer is behind. */
    void pushVideo (const juce::Image& argb, double ptsSeconds);

    /** Waits for everything queued to be written, closes the file. */
    bool finish (juce::String& error);

    int64_t audioFramesPushed() const noexcept { return pushed.load(); }
    int videoFramesWritten() const noexcept { return framesOut.load(); }
    int videoFramesDropped() const noexcept { return videoDrops.load(); }
    uint64_t audioDropped() const noexcept { return ring ? ring->numDropped() : 0; }
    juce::File file() const { return settings.file; }

private:
    void run() override;
    bool drain (bool everything);

    struct Picture { std::vector<uint8_t> bgra; double pts = 0.0; };

    ExportSettings settings;
    std::unique_ptr<ExportBackend> backend;
    std::unique_ptr<SpscRing<float>> ring;   // interleaved stereo
    std::atomic<bool> running { false };
    std::atomic<bool> finishing { false };
    std::atomic<int64_t> pushed { 0 };
    int64_t written = 0;
    std::atomic<int> framesOut { 0 }, videoDrops { 0 };
    std::mutex pictureLock;
    std::deque<Picture> pictures;
    std::vector<int16_t> pcm;
    std::vector<float> chunk;
    juce::String problem;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GridExporter)
};

} // namespace griddy
