#pragma once

#include "SpscRing.h"
#include "TakeHistory.h"
#include "sync/EmbeddedSync.h"

#include <juce_graphics/juce_graphics.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

namespace griddy
{

/** Writes one take while the DAW records.

    Threads:
      audio thread   feedAudio(): copies samples and a timestamp pair into lock free rings
      camera thread  pushFrame(): queues the newest frame (the oldest is dropped if the writer falls behind)
      writer thread  encodes JPEGs, appends the reference audio, writes files
      control thread start() and stop()
    A take is crash safe: every frame is its own file, the WAV header is refreshed as it grows, and
    take.json is rewritten atomically at the end. A take with state "recording" on disk is a crashed take.
*/
class TakeRecorder
{
public:
    struct Config
    {
        int width = 1280, height = 720;
        int proxyWidth = 640, proxyHeight = 360;
        float jpegQuality = 0.85f;
        bool embedSync = false;         // Griddy written audio: mix the sync signal into audio.wav
        double cameraLatencySeconds = 0.0;
        double audioLatencySeconds = 0.0;
        double fps = 30.0;
        bool exportMov = true;          // write take.mov after the take stops (needs FFmpeg)
        juce::File flatFolder;          // if set, take.mov is also copied here as "Slot N Take M.mov"
    };

    explicit TakeRecorder (TakeHistory& history);
    ~TakeRecorder();

    bool start (int slot, TakeMeta initial, const Config& config);

    /** Audio thread. hostSample is the host time of mono[0]. */
    void feedAudio (const float* mono, int numSamples, int64_t hostSample) noexcept;

    /** Camera thread. captureWallSeconds is steadyNowSeconds() at capture. */
    void pushFrame (juce::Image frame, double captureWallSeconds);

    /** Stops, finalises and returns the take (now in review). */
    std::optional<TakeHistory::Entry> stop();

    /** Blocks until the take.mov of the last stopped take is written (or failed). Returns true if it exists. */
    bool waitForMovExport();
    juce::String movExportError() const { return movError; }

    /** The plainly named copy of the last take.mov, once written (empty when none). */
    juce::File flatCopy() const;

    bool isRecording() const noexcept { return recording.load(); }
    int32_t currentTakeId() const noexcept { return entry.meta.takeId; }
    uint64_t framesWritten() const noexcept { return written.load(); }
    uint64_t framesDropped() const noexcept { return droppedFrames.load(); }
    uint64_t audioDropped() const noexcept { return audioRing.numDropped(); }

private:
    struct Pending { juce::Image image; double captureWall; double arrivalWall; };
    struct Stamp { double wall; int64_t hostSample; int32_t numSamples; };

    class Writer;

    TakeHistory& history;
    SpscRing<float> audioRing { 48000 * 6 };
    SpscRing<Stamp> stampRing { 1 << 16 };

    std::mutex frameLock;
    std::deque<Pending> frames;

    TakeHistory::Entry entry;
    Config config;
    std::atomic<bool> recording { false };
    std::atomic<bool> stopRequested { false };
    std::atomic<uint64_t> written { 0 }, droppedFrames { 0 };
    std::atomic<int64_t> firstHost { -1 };   // host sample of the first block actually fed (the audio thread may start late)
    std::unique_ptr<Writer> writer;
    std::unique_ptr<juce::Thread> movExport;
    juce::String movError;
};

} // namespace griddy
