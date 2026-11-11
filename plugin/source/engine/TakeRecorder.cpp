#include "TakeRecorder.h"

#include "MovTake.h"
#include "HostClock.h"

#include <algorithm>
#include <cmath>

namespace griddy
{

class TakeRecorder::Writer final : public juce::Thread
{
public:
    Writer (TakeRecorder& o) : juce::Thread ("Griddy take writer"), owner (o) {}

    void run() override
    {
        const auto& cfg = owner.config;
        WavWriter wav;
        wav.open (owner.entry.folder.getChildFile ("audio.wav"), owner.entry.meta.sampleRate);

        std::unique_ptr<embedded::Encoder> enc;
        if (cfg.embedSync)
            enc = std::make_unique<embedded::Encoder> (owner.entry.meta.sampleRate, (uint8_t) owner.entry.meta.embeddedTag);

        juce::JPEGImageFormat jpeg;
        jpeg.setQuality (cfg.jpegQuality);
        juce::Image proxy (juce::Image::ARGB, cfg.proxyWidth, cfg.proxyHeight, false, juce::SoftwareImageType());
        juce::Image full (juce::Image::ARGB, cfg.width, cfg.height, false, juce::SoftwareImageType());

        std::vector<float> chunk (8192);
        std::vector<float> allAudioForEnvelope;
        std::vector<double> stampWall, stampHost;
        std::vector<double> frameWall, deliveryDelays;
        int frameIndex = 0;

        auto drainAudio = [&]
        {
            for (;;)
            {
                const auto n = owner.audioRing.read (chunk.data(), chunk.size());
                if (n == 0)
                    break;

                if (enc != nullptr)
                    enc->mixInto (chunk.data(), (int) n);

                wav.write (chunk.data(), (int) n);
                allAudioForEnvelope.insert (allAudioForEnvelope.end(), chunk.begin(), chunk.begin() + (long) n);
            }

            Stamp s;
            while (owner.stampRing.read (&s, 1) == 1)
            {
                stampWall.push_back (s.wall);
                stampHost.push_back ((double) s.hostSample / owner.entry.meta.sampleRate);
            }
        };

        auto drainFrames = [&] (bool all)
        {
            for (int guard = 0; guard < (all ? 1000000 : 4); ++guard)
            {
                Pending p;
                {
                    const std::scoped_lock sl (owner.frameLock);
                    if (owner.frames.empty())
                        return;
                    p = std::move (owner.frames.front());
                    owner.frames.pop_front();
                }

                auto name = juce::String::formatted ("%06d.jpg", frameIndex);
                auto scaled = [&] (juce::Image& dst, const juce::Image& src)
                {
                    juce::Graphics g (dst);
                    g.setImageResamplingQuality (juce::Graphics::mediumResamplingQuality);
                    g.drawImage (src, dst.getBounds().toFloat());
                };

                scaled (full, p.image);
                scaled (proxy, p.image);

                auto writeJpeg = [&] (const char* sub, juce::Image& img)
                {
                    juce::FileOutputStream out (owner.entry.folder.getChildFile (sub).getChildFile (name));
                    if (out.openedOk())
                        jpeg.writeImageToStream (img, out);
                };

                writeJpeg ("frames", full);
                writeJpeg ("proxy", proxy);

                frameWall.push_back (p.captureWall);
                deliveryDelays.push_back (p.arrivalWall - p.captureWall);
                ++frameIndex;
                ++owner.written;
            }
        };

        while (! threadShouldExit() && ! owner.stopRequested.load())
        {
            drainAudio();
            drainFrames (false);
            wait (3);
        }

        drainAudio();
        drainFrames (true);
        wav.close();

        // Recording is started from a control thread, so the first block fed may come after the one that asked for it.
        // The audio file begins at the first block that was really written.
        if (const auto first = owner.firstHost.load(); first >= 0 && first != owner.entry.meta.startHostSample)
        {
            auto& m = owner.entry.meta;
            m.startPpq += (double) (first - m.startHostSample) / m.sampleRate * m.bpm / 60.0;
            m.startHostSample = first;
        }

        // Map camera capture times onto the host timeline. The audio thread stamped every block
        // with (wall clock, host sample), so a straight line fit through them gives the host time
        // of any wall clock moment with the block to block jitter averaged out.
        auto& meta = owner.entry.meta;
        double a = 0.0, b = 1.0;
        if (stampWall.size() >= 2)
        {
            const auto n = (double) stampWall.size();
            double sw = 0, sh = 0, sww = 0, swh = 0;
            const auto w0 = stampWall.front();

            for (size_t i = 0; i < stampWall.size(); ++i)
            {
                const auto w = stampWall[i] - w0;
                sw += w; sh += stampHost[i]; sww += w * w; swh += w * stampHost[i];
            }

            const auto den = n * sww - sw * sw;
            b = den > 1.0e-12 ? (n * swh - sw * sh) / den : 1.0;
            a = (sh - b * sw) / n - b * 0.0;
            // host = a + b * (wall - w0)
            for (auto& w : frameWall)
                w = a + b * (w - w0);
        }
        else if (! frameWall.empty())
        {
            for (auto& w : frameWall)
                w = 0.0;
        }

        const auto startHostSeconds = meta.hostStartSeconds();
        meta.frameTimes.clear();
        for (auto hw : frameWall)
            meta.frameTimes.push_back (hw - startHostSeconds);

        std::sort (deliveryDelays.begin(), deliveryDelays.end());
        meta.deliveryDelaySeconds = deliveryDelays.empty() ? 0.0 : deliveryDelays[deliveryDelays.size() / 2];
        meta.cameraLatencySeconds = cfg.cameraLatencySeconds;
        meta.audioLatencySeconds = cfg.audioLatencySeconds;
        meta.numFrames = frameIndex;
        meta.durationSeconds = (double) wav.numWritten() / meta.sampleRate;
        meta.envelope = computeEnvelope (allAudioForEnvelope.data(), allAudioForEnvelope.size(), meta.sampleRate);
        meta.embeddedSync = cfg.embedSync;
    }

private:
    TakeRecorder& owner;
};

//==============================================================================
TakeRecorder::TakeRecorder (TakeHistory& h) : history (h) {}

TakeRecorder::~TakeRecorder()
{
    if (recording.load())
        stop();

    if (movExport != nullptr)
        movExport->stopThread (60000);
}

namespace
{
class MovExportThread final : public juce::Thread
{
public:
    MovExportThread (TakeMeta m, juce::File f) : juce::Thread ("take.mov export"), meta (std::move (m)), folder (std::move (f)) {}

    void run() override
    {
        ran = true;
        juce::String error;
        ok = exportTakeMov (meta, folder, folder.getChildFile ("take.mov"), error);
        message = error;
    }

    TakeMeta meta;
    juce::File folder;
    bool ok = false, ran = false;
    juce::String message;
};
} // namespace

bool TakeRecorder::waitForMovExport()
{
    if (movExport == nullptr)
    {
        movError = juce::String ("export thread was not started (exportMov ") + (config.exportMov ? "on" : "off") + ", exporter " + (movExportAvailable() ? "available" : "missing") + ")";
        return false;
    }

    movExport->waitForThreadToExit (120000);
    movError = static_cast<MovExportThread&> (*movExport).message;
    if (! static_cast<MovExportThread&> (*movExport).ran)
        movError = "export thread never ran";
    else if (movError.isEmpty() && ! static_cast<MovExportThread&> (*movExport).ok)
        movError = "exporter returned false without a message";
    return static_cast<MovExportThread&> (*movExport).ok;
}

bool TakeRecorder::start (int slot, TakeMeta initial, const Config& cfg)
{
    if (recording.exchange (true))
        return false;

    config = cfg;
    initial.width = cfg.width;
    initial.height = cfg.height;
    initial.proxyWidth = cfg.proxyWidth;
    initial.proxyHeight = cfg.proxyHeight;
    initial.fps = cfg.fps;
    entry = history.beginTake (slot, std::move (initial));

    // Drop anything left from the previous take.
    float scratch[256];
    while (audioRing.read (scratch, 256) > 0) {}
    Stamp st;
    while (stampRing.read (&st, 1) > 0) {}
    {
        const std::scoped_lock sl (frameLock);
        frames.clear();
    }

    firstHost = -1;
    stopRequested = false;
    written = 0;
    droppedFrames = 0;
    writer = std::make_unique<Writer> (*this);
    writer->startThread (juce::Thread::Priority::normal);
    return true;
}

void TakeRecorder::feedAudio (const float* mono, int numSamples, int64_t hostSample) noexcept
{
    if (! recording.load (std::memory_order_relaxed))
        return;

    auto none = int64_t (-1);
    firstHost.compare_exchange_strong (none, hostSample, std::memory_order_relaxed);
    audioRing.write (mono, (size_t) numSamples);
    Stamp s { steadyNowSeconds(), hostSample };
    stampRing.write (&s, 1);
}

void TakeRecorder::pushFrame (juce::Image frame, double captureWallSeconds)
{
    if (! recording.load())
        return;

    const std::scoped_lock sl (frameLock);

    if (frames.size() >= 12)
    {
        frames.pop_front();
        ++droppedFrames;
    }

    frames.push_back ({ std::move (frame), captureWallSeconds, steadyNowSeconds() });
}

std::optional<TakeHistory::Entry> TakeRecorder::stop()
{
    if (! recording.load())
        return std::nullopt;

    stopRequested = true;
    writer->stopThread (60000);
    writer.reset();
    recording = false;

    history.finishTake (entry);

    if (config.exportMov && movExportAvailable())
    {
        if (movExport != nullptr)
            movExport->stopThread (60000);

        // A copy of the final meta: the export must not read the entry while the UI edits it.
        auto t = std::make_unique<MovExportThread> (entry.meta, entry.folder);
        const auto started = t->startThread (juce::Thread::Priority::normal);
        if (! started)
            t->run(); // no thread to be had: write it here rather than not at all
        movExport = std::move (t);
    }

    return entry;
}

} // namespace griddy
