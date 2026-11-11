#include "VideoEngine.h"

namespace griddy
{

static double nowSeconds() { return juce::Time::getMillisecondCounterHiRes() / 1000.0; }

static int readChoice (std::atomic<float>* p, int fallback)
{
    return p != nullptr ? juce::roundToInt (p->load (std::memory_order_relaxed)) : fallback;
}

VideoEngine::VideoEngine (TransportState& t, EngineControls c)
    : juce::Thread ("Griddy video engine"), transport (t), controls (c)
{
    outputFolder = juce::File::getSpecialLocation (juce::File::userMoviesDirectory).getChildFile ("Griddy");
    previewFormat.setQuality (0.7f);
    camerasAllowed = hostAllowsCameraAccess (cameraBlockedReason);
    recorderFormat = createRecorder()->getFormatName();
}

VideoEngine::~VideoEngine()
{
    stopEngine();

    const std::scoped_lock sl (slotLock);
    sources.clear(); // stops every camera
}

void VideoEngine::startEngine()
{
    if (! isThreadRunning())
        startThread (juce::Thread::Priority::high);
}

void VideoEngine::stopEngine()
{
    stopThread (3000);

    // Never leave a half written file behind.
    if (recorder != nullptr)
    {
        recorder->finish();
        recorder.reset();
        recording = false;
    }
}

juce::String VideoEngine::setSlotSource (int slot, const juce::String& sourceId)
{
    if (! juce::isPositiveAndBelow (slot, maxSlots))
        return "No slot " + juce::String (slot + 1);

    juce::String error;

    {
        const std::scoped_lock sl (slotLock);

        // Two slots can share one camera: only open it the first time.
        if (sourceId.isNotEmpty() && sources.find (sourceId) == sources.end())
        {
            auto source = createCaptureSource (sourceId, frameStore);

            if (source == nullptr)
                return camerasAllowed || sourceId.startsWith ("test:") ? "That source isn't available anymore"
                                                                         : cameraBlockedReason;

            if (! source->start (error))
                return error;

            sources[sourceId] = std::move (source);
        }

        slotSources[(size_t) slot] = sourceId;
    }

    releaseUnusedSources();
    return {};
}

void VideoEngine::releaseUnusedSources()
{
    std::vector<std::unique_ptr<CaptureSource>> toStop;

    {
        const std::scoped_lock sl (slotLock);

        for (auto it = sources.begin(); it != sources.end();)
        {
            const auto used = std::find (slotSources.begin(), slotSources.end(), it->first) != slotSources.end();

            if (used)
            {
                ++it;
            }
            else
            {
                toStop.push_back (std::move (it->second));
                it = sources.erase (it);
            }
        }
    }

    // Stopping a camera can block briefly, so do it outside the lock.
    toStop.clear();
}

std::array<juce::String, VideoEngine::maxSlots> VideoEngine::getSlotSources() const
{
    const std::scoped_lock sl (slotLock);
    return slotSources;
}

void VideoEngine::setOutputFolder (const juce::File& folder)
{
    const std::scoped_lock sl (statusLock);
    outputFolder = folder;
}

juce::File VideoEngine::getOutputFolder() const
{
    const std::scoped_lock sl (statusLock);
    return outputFolder;
}

juce::var VideoEngine::getStatus() const
{
    auto obj = std::make_unique<juce::DynamicObject>();

    {
        const std::scoped_lock sl (statusLock);
        obj->setProperty ("outputFolder", outputFolder.getFullPathName());
        obj->setProperty ("lastTake", lastTakePath);
        obj->setProperty ("lastError", lastError);
        obj->setProperty ("recorderFormat", recorderFormat);
    }

    obj->setProperty ("recording", recording.load());
    obj->setProperty ("fps", measuredFps.load());
    obj->setProperty ("camerasAllowed", camerasAllowed);
    obj->setProperty ("cameraBlockedReason", cameraBlockedReason);
    obj->setProperty ("outputSize", juce::String (outputWidth) + "x" + juce::String (outputHeight));
    return juce::var (obj.release());
}

juce::MemoryBlock VideoEngine::getPreviewJpeg() const
{
    const std::scoped_lock sl (previewLock);
    return previewJpeg;
}

//==============================================================================
void VideoEngine::run()
{
    const auto frameDuration = 1.0 / fps;
    auto nextFrameTime = nowSeconds();
    auto fpsWindowStart = nextFrameTime;
    int fpsWindowFrames = 0;

    while (! threadShouldExit())
    {
        const auto now = nowSeconds();
        renderOneFrame (now);

        ++fpsWindowFrames;
        if (now - fpsWindowStart >= 1.0)
        {
            measuredFps = fpsWindowFrames / (now - fpsWindowStart);
            fpsWindowStart = now;
            fpsWindowFrames = 0;
        }

        // Fixed timestep: aim for exact 1/30 s spacing so the recorded file
        // plays back at real speed. If we fall badly behind, reset rather than burst.
        nextFrameTime += frameDuration;
        const auto waitMs = (int) std::floor ((nextFrameTime - nowSeconds()) * 1000.0);

        if (waitMs > 0)
            wait (waitMs);
        else if (waitMs < -250)
            nextFrameTime = nowSeconds();
    }
}

void VideoEngine::renderOneFrame (double now)
{
    const auto t = transport.read();
    beatClock.update (t, now);

    const auto mode = (LayoutMode) juce::jlimit (0, 2, readChoice (controls.layout, 0));
    const auto rate = (SwitchRate) juce::jlimit (0, 4, readChoice (controls.switchRate, 2));
    const auto slots = juce::jlimit (1, maxSlots, readChoice (controls.numSlots, 4));

    const auto cells = computeLayout (mode, slots, beatClock.getStepIndex (rate));
    const auto assigned = getSlotSources();

    compositor.render (output, cells, [&] (int slot) -> std::shared_ptr<const VideoFrame>
    {
        const auto& id = assigned[(size_t) slot];
        return id.isNotEmpty() ? frameStore.latest (id) : nullptr;
    });

    // Recording
    const auto armed = readChoice (controls.recordArm, 0) != 0;
    const auto recordMode = (RecordMode) juce::jlimit (0, 2, readChoice (controls.recordMode, 0));
    const auto shouldRecord = armed && (recordMode == RecordMode::manual
                                        || (recordMode == RecordMode::followDawPlay && t.isPlaying)
                                        || (recordMode == RecordMode::followDawRecord && t.isPlaying && t.isRecording));
    updateRecording (shouldRecord, t);

    if (recorder != nullptr)
        recorder->writeFrame (output, takeFrameIndex++);

    // 15 fps previews are smooth enough to aim cameras and half the cost of 30.
    if ((framesRendered++ % 2) == 0)
        encodePreview (output);
}

void VideoEngine::updateRecording (bool shouldRecord, const TransportSnapshot& t)
{
    if (shouldRecord && recorder == nullptr)
    {
        TakeInfo info;
        info.startPpq = beatClock.getPositionInQuarterNotes();
        info.bpm = t.bpm;
        info.timeSigNumerator = t.timeSigNumerator;
        info.timeSigDenominator = t.timeSigDenominator;
        info.fps = fps;
        info.width = outputWidth;
        info.height = outputHeight;

        const auto folder = getOutputFolder();
        folder.createDirectory();

        const auto barLen = beatClock.getBarLengthInQuarterNotes();
        const auto bar = (int) std::floor (info.startPpq / barLen) + 1;
        const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
        const auto base = folder.getNonexistentChildFile ("Griddy " + stamp + " bar " + juce::String (bar), {}, false);

        auto r = createRecorder();
        juce::String error;

        if (r->start (base, info, error))
        {
            recorder = std::move (r);
            takeFrameIndex = 0;
            recording = true;
            const std::scoped_lock sl (statusLock);
            lastError = {};
        }
        else
        {
            const std::scoped_lock sl (statusLock);
            lastError = error;
        }
    }
    else if (! shouldRecord && recorder != nullptr)
    {
        const auto file = recorder->finish();
        recorder.reset();
        recording = false;

        const std::scoped_lock sl (statusLock);
        lastTakePath = file.getFullPathName();
    }
}

void VideoEngine::encodePreview (const juce::Image& frame)
{
    {
        juce::Graphics g (previewImage);
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImage (frame, previewImage.getBounds().toFloat());
    }

    juce::MemoryOutputStream out;

    if (previewFormat.writeImageToStream (previewImage, out))
    {
        {
            const std::scoped_lock sl (previewLock);
            previewJpeg = out.getMemoryBlock();
        }

        ++previewSerial;
    }
}

} // namespace griddy
