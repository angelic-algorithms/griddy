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
        std::vector<double> stampWall, stampTake;
        int64_t stampedSamples = 0;
        std::vector<TakeMeta::HostPiece> pieces;
        int64_t expectedHost = 0;
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
                const auto sr = owner.entry.meta.sampleRate;

                // A new host piece wherever the host position jumps (a loop wrap while recording).
                if (pieces.empty() || std::abs (s.hostSample - expectedHost) > 32)
                    pieces.push_back ({ (double) stampedSamples / sr, (double) s.hostSample / sr });
                expectedHost = s.hostSample + s.numSamples;

                stampWall.push_back (s.wall);
                stampTake.push_back ((double) stampedSamples / sr);
                stampedSamples += s.numSamples;
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

        // Map camera capture times onto the take's own audio. The audio thread stamped every block with
        // the wall clock, and the blocks are the take audio in order, so a straight line fit through
        // (wall clock, seconds into the take audio) gives the take time of any frame with the block to
        // block jitter averaged out. The host position is not used: when the DAW loops while recording,
        // it jumps back at every wrap, but the take audio and the frames keep running forward.
        auto& meta = owner.entry.meta;
        meta.frameTimes.clear();

        if (stampWall.size() >= 2)
        {
            const auto n = (double) stampWall.size();
            double sw = 0, st = 0, sww = 0, swt = 0;
            const auto w0 = stampWall.front();

            for (size_t i = 0; i < stampWall.size(); ++i)
            {
                const auto w = stampWall[i] - w0;
                sw += w; st += stampTake[i]; sww += w * w; swt += w * stampTake[i];
            }

            const auto den = n * sww - sw * sw;
            auto b = den > 1.0e-12 ? (n * swt - sw * st) / den : 1.0;

            // Audio runs in real time, so anything far from 1 is a broken fit (a stall, a dropout): use real time.
            if (b < 0.9 || b > 1.1)
                b = 1.0;

            const auto a = (st - b * sw) / n;
            for (auto w : frameWall)
                meta.frameTimes.push_back (a + b * (w - w0));
        }
        else
        {
            for (size_t i = 0; i < frameWall.size(); ++i)
                meta.frameTimes.push_back ((double) i / cfg.fps);
        }

        std::sort (deliveryDelays.begin(), deliveryDelays.end());
        meta.deliveryDelaySeconds = deliveryDelays.empty() ? 0.0 : deliveryDelays[deliveryDelays.size() / 2];
        meta.cameraLatencySeconds = cfg.cameraLatencySeconds;
        meta.audioLatencySeconds = cfg.audioLatencySeconds;
        meta.numFrames = frameIndex;
        meta.durationSeconds = (double) wav.numWritten() / meta.sampleRate;
        meta.envelope = computeEnvelope (allAudioForEnvelope.data(), allAudioForEnvelope.size(), meta.sampleRate);
        meta.embeddedSync = cfg.embedSync;
        meta.hostPieces = pieces.size() > 1 ? pieces : std::vector<TakeMeta::HostPiece> {};
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
    MovExportThread (TakeMeta m, juce::File f, juce::File flat)
        : juce::Thread ("take.mov export"), meta (std::move (m)), folder (std::move (f)), flatFolder (std::move (flat)) {}

    void run() override
    {
        ran = true;
        juce::String error;
        ok = exportTakeMov (meta, folder, folder.getChildFile ("take.mov"), error);
        message = error;

        // A plain copy in one flat folder, so takes can be found in Finder without the session folders.
        // Two slots can finish at once; whichever creates the folder second gets an error but finds it there.
        if (ok && flatFolder != juce::File())
            flatFolder.createDirectory();

        if (ok && flatFolder.isDirectory())
        {
            const auto name = "Slot " + juce::String (meta.slot + 1) + " Take " + juce::String (meta.takeId) + ".mov";
            const auto target = flatFolder.getChildFile (name).getNonexistentSibling (true);
            if (folder.getChildFile ("take.mov").copyFileTo (target))
                copied = target;
        }
    }

    TakeMeta meta;
    juce::File folder, flatFolder, copied;
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

juce::File TakeRecorder::flatCopy() const
{
    return movExport != nullptr ? static_cast<MovExportThread&> (*movExport).copied : juce::File();
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
    Stamp s { steadyNowSeconds(), hostSample, (int32_t) numSamples };
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
        auto t = std::make_unique<MovExportThread> (entry.meta, entry.folder, config.flatFolder);
        const auto started = t->startThread (juce::Thread::Priority::normal);
        if (! started)
            t->run(); // no thread to be had: write it here rather than not at all
        movExport = std::move (t);
    }

    return entry;
}

} // namespace griddy
