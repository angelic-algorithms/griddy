#include "EngineHost.h"

#include "CollageLayout.h"

namespace griddy
{

static int readInt (const std::atomic<float>* p, int fallback)
{
    return p != nullptr ? (int) std::lround (p->load()) : fallback;
}

EngineHost::EngineHost (VideoEngine& v, EngineControls c)
    : juce::Thread ("Griddy control"), video (v), controls (c)
{
    // The engine draws the picture from here on: live cameras where a slot has no take, takes where it has.
    video.setComposeEnabled (false);
    sessionId = juce::Uuid().toDashedString();
    startThread (juce::Thread::Priority::normal);
}

EngineHost::~EngineHost()
{
    stopThread (5000);
    worker.removeAllJobs (true, 60000);
    destroyEngine();
    video.setComposeEnabled (true);
}

//==============================================================================
void EngineHost::buildEngine()
{
    PlaybackEngine::Config cfg;
    cfg.numSlots = maxSlots;
    cfg.sampleRate = sampleRate;
    cfg.canvas = Canvas::hd720;
    cfg.runRenderThread = false; // this class draws the preview, including the live camera
    cfg.takesRoot = video.getOutputFolder().getChildFile ("Sessions").getChildFile (sessionId);
    cfg.takesRoot.createDirectory();

    auto e = std::make_unique<PlaybackEngine> (cfg);
    e->start();

    auto r = std::make_unique<TakeRecorder> (e->history());

    {
        const std::scoped_lock sl (engineLock);
        engine = std::move (e);
        recorder = std::move (r);
        audioEngine = engine.get();
        audioRecorder = recorder.get();
    }

    // Takes from earlier runs of this project: fingerprint them so old clips find their video.
    worker.addJob ([this]
    {
        for (int s = 0; s < maxSlots; ++s)
        {
            PlaybackEngine* e2 = nullptr;
            {
                const std::scoped_lock sl (engineLock);
                e2 = engine.get();
            }

            if (e2 == nullptr)
                return;

            if (! e2->history().playable (s).empty())
                e2->reloadSlot (s);
        }
    });
}

void EngineHost::destroyEngine()
{
    // The audio thread reads these two pointers. Withdraw them, give any block that already started
    // time to finish, then destroy. (The host does not call processBlock during prepareToPlay.)
    audioEngine = nullptr;
    audioRecorder = nullptr;
    juce::Thread::sleep (40);

    std::unique_ptr<PlaybackEngine> e;
    std::unique_ptr<TakeRecorder> r;
    {
        const std::scoped_lock sl (engineLock);
        r = std::move (recorder);
        e = std::move (engine);
    }

    if (r != nullptr && r->isRecording())
        r->stop();

    if (e != nullptr)
        e->stop();
}

void EngineHost::prepare (double sr, int maxBlockSize)
{
    worker.removeAllJobs (true, 60000);
    const auto changed = sr != sampleRate || (engine == nullptr);
    sampleRate = sr;
    maxBlock = juce::jmax (64, maxBlockSize);

    mono.assign ((size_t) maxBlock, 0.0f);
    silence.assign ((size_t) maxBlock, 0.0f);

    if (changed)
    {
        destroyEngine();
        buildEngine();
    }
}

void EngineHost::setSession (const juce::String& id)
{
    if (id.isEmpty() || id == sessionId)
        return;

    worker.removeAllJobs (true, 60000);
    destroyEngine();
    sessionId = id;
    buildEngine();
}

juce::String EngineHost::getSession() const
{
    return sessionId;
}

bool EngineHost::isRecording() const noexcept
{
    return recordingSlot.load() >= 0;
}

//==============================================================================
void EngineHost::processBlock (const juce::AudioBuffer<float>& buffer, const TransportSnapshot& t) noexcept
{
    auto* e = audioEngine.load (std::memory_order_acquire);
    auto* r = audioRecorder.load (std::memory_order_acquire);

    const auto channels = buffer.getNumChannels();
    const auto total = buffer.getNumSamples();

    // Decide whether this is a moment to record. Arm gates everything, so a stray play never records over a take.
    const auto armed = controls.recordArm != nullptr && controls.recordArm->load() > 0.5f;
    const auto mode = readInt (controls.recordMode, 0);
    const auto want = armed && t.isPlaying && (mode == 0 ? t.isRecording : true);

    if (want && ! wantRecording.load (std::memory_order_relaxed))
    {
        requestSample = t.timeInSamples;
        requestPpq = t.ppqPosition;
        requestBpm = t.bpm;
        requestNum = t.timeSigNumerator;
        requestDen = t.timeSigDenominator;
    }

    wantRecording.store (want, std::memory_order_release);
    playingNow.store (t.isPlaying, std::memory_order_relaxed);

    if (e == nullptr)
        return;

    const auto active = juce::jlimit (1, maxSlots, activeSlots.load (std::memory_order_relaxed));
    const auto tgt = juce::jmin (target.load (std::memory_order_relaxed), active - 1);

    for (int done = 0; done < total;)
    {
        const auto n = juce::jmin (total - done, maxBlock);
        const auto host = t.timeInSamples + done;

        // The track's audio as one channel.
        if (channels <= 0)
            std::fill (mono.begin(), mono.begin() + n, 0.0f);
        else
        {
            const auto* c0 = buffer.getReadPointer (0) + done;
            if (channels == 1)
                std::copy (c0, c0 + n, mono.begin());
            else
            {
                const auto* c1 = buffer.getReadPointer (1) + done;
                for (int i = 0; i < n; ++i)
                    mono[(size_t) i] = 0.5f * (c0[i] + c1[i]);
            }
        }

        for (int s = 0; s < active; ++s)
            e->feedSlotAudio (s, s == tgt ? mono.data() : silence.data(), n, host, t.isPlaying);

        if (r != nullptr && r->isRecording())
            r->feedAudio (mono.data(), n, host);

        done += n;
    }
}

//==============================================================================
void EngineHost::run()
{
    while (! threadShouldExit())
    {
        controlTick();
        wait (4);
    }
}

void EngineHost::controlTick()
{
    PlaybackEngine* e = nullptr;
    TakeRecorder* r = nullptr;
    {
        const std::scoped_lock sl (engineLock);
        e = engine.get();
        r = recorder.get();
    }

    if (e == nullptr || r == nullptr)
        return;

    activeSlots = juce::jlimit (1, maxSlots, readInt (controls.numSlots, 4));

    const auto want = wantRecording.load (std::memory_order_acquire);

    if (want && ! r->isRecording() && ! workerBusy.load())
    {
        const auto slot = juce::jmin (target.load(), activeSlots.load() - 1);

        TakeMeta meta;
        meta.sampleRate = sampleRate;
        meta.startHostSample = requestSample.load();
        meta.startPpq = requestPpq.load();
        meta.bpm = requestBpm.load();
        meta.timeSigNumerator = requestNum.load();
        meta.timeSigDenominator = requestDen.load();

        TakeRecorder::Config cfg;
        cfg.embedSync = false;

        if (r->start (slot, meta, cfg))
        {
            recordingSlot = slot;
            lastCameraSerial = 0;
        }
    }
    else if (! want && r->isRecording() && ! workerBusy.load())
    {
        const auto slot = recordingSlot.load();
        workerBusy = true;
        // Stopping writes the last files and the take.json; rebuilding the slot analyses its audio.
        worker.addJob ([this, slot]
        {
            finishTake (slot);
            workerBusy = false;
        });
    }

    // Camera frames go to the recorder as they arrive. The recorder timestamps them by capture time.
    if (r->isRecording())
    {
        const auto slot = recordingSlot.load();
        if (auto frame = video.latestFrame (slot); frame != nullptr && (uint64_t) frame->serial != lastCameraSerial)
        {
            lastCameraSerial = (uint64_t) frame->serial;
            r->pushFrame (frame->image, frame->captureTimeSeconds);
        }
    }

    // 15 fps preview.
    if ((tick++ % 8) == 0)
        drawPreview();
}

void EngineHost::finishTake (int slot)
{
    PlaybackEngine* e = nullptr;
    TakeRecorder* r = nullptr;
    {
        const std::scoped_lock sl (engineLock);
        e = engine.get();
        r = recorder.get();
    }

    if (e == nullptr || r == nullptr)
        return;

    if (const auto entry = r->stop())
    {
        droppedFrames += r->framesDropped();
        droppedAudio += r->audioDropped();
        recordingSlot = -1;
        e->reloadSlot (slot);
    }
    else
    {
        recordingSlot = -1;
        const std::scoped_lock sl (statusLock);
        lastError = "A take could not be finished.";
    }
}

void EngineHost::drawPreview()
{
    PlaybackEngine* e = nullptr;
    {
        const std::scoped_lock sl (engineLock);
        e = engine.get();
    }

    if (e == nullptr)
        return;

    const auto active = activeSlots.load();
    const auto cells = computeLayout (active);
    const auto recSlot = recordingSlot.load();

    compositor.render (output, cells, [&] (int slot) -> std::shared_ptr<const VideoFrame>
    {
        // The slot being recorded shows the live camera. Others show their take, or the live camera if they have none.
        if (slot == recSlot)
            return video.latestFrame (slot);

        if (const auto shown = e->shown (slot); shown != nullptr && shown->frame != nullptr)
            return shown->frame;

        return video.latestFrame (slot);
    });

    {
        juce::Graphics g (previewImage);
        g.setImageResamplingQuality (juce::Graphics::lowResamplingQuality);
        g.drawImage (output, previewImage.getBounds().toFloat());
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

juce::MemoryBlock EngineHost::getPreviewJpeg() const
{
    const std::scoped_lock sl (previewLock);
    return previewJpeg;
}

//==============================================================================
static const char* statusName (SyncStatus s)
{
    switch (s)
    {
        case SyncStatus::noTake:  return "noTake";
        case SyncStatus::stopped: return "stopped";
        case SyncStatus::prior:   return "prior";
        case SyncStatus::locked:  return "locked";
        case SyncStatus::held:    return "held";
        case SyncStatus::lost:    return "lost";
    }
    return "noTake";
}

static const char* kindName (SyncKind k)
{
    switch (k)
    {
        case SyncKind::embedded:    return "embedded";
        case SyncKind::fingerprint: return "fingerprint";
        case SyncKind::hostPosition: return "hostPosition";
        default: break;
    }
    return "none";
}

juce::var EngineHost::takesToVar (int slot) const
{
    juce::Array<juce::var> list;
    const std::scoped_lock sl (engineLock);

    if (engine == nullptr)
        return list;

    for (const auto& e : engine->history().listed (slot))
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("id", e.meta.takeId);
        o->setProperty ("state", e.meta.state == TakeMeta::State::kept ? "kept"
                               : e.meta.state == TakeMeta::State::review ? "review" : "archived");
        o->setProperty ("seconds", e.meta.durationSeconds);
        o->setProperty ("slip", e.meta.slipSeconds);
        o->setProperty ("hasMov", e.folder.getChildFile ("take.mov").existsAsFile());
        list.add (juce::var (o.release()));
    }

    return list;
}

juce::var EngineHost::getStatus() const
{
    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty ("session", sessionId);
    obj->setProperty ("targetSlot", target.load());
    obj->setProperty ("recording", recordingSlot.load() >= 0);
    obj->setProperty ("recordingSlot", recordingSlot.load());
    obj->setProperty ("armed", controls.recordArm != nullptr && controls.recordArm->load() > 0.5f);
    obj->setProperty ("sampleRate", sampleRate);

    {
        const std::scoped_lock sl (statusLock);
        obj->setProperty ("error", lastError);
    }

    juce::Array<juce::var> slotList;
    {
        const std::scoped_lock sl (engineLock);
        if (engine != nullptr)
        {
            const auto active = activeSlots.load();
            for (int s = 0; s < active; ++s)
            {
                auto o = std::make_unique<juce::DynamicObject>();
                const auto shown = engine->shown (s);
                o->setProperty ("slot", s);
                o->setProperty ("status", shown != nullptr ? statusName (shown->status) : "noTake");
                o->setProperty ("source", shown != nullptr ? kindName (shown->kind) : "none");
                o->setProperty ("takeId", shown != nullptr ? shown->takeId : -1);
                o->setProperty ("warp", engine->slotLooksWarped (s));
                o->setProperty ("lockMs", engine->lockLatencySeconds (s) * 1000.0);
                slotList.add (juce::var (o.release()));
            }
        }
    }
    obj->setProperty ("slots", slotList);

    juce::Array<juce::var> takes;
    for (int s = 0; s < activeSlots.load(); ++s)
        takes.add (takesToVar (s));
    obj->setProperty ("takes", takes);

    if (auto* r = audioRecorder.load())
    {
        obj->setProperty ("framesWritten", (juce::int64) r->framesWritten());
        obj->setProperty ("framesDropped", (juce::int64) (droppedFrames + r->framesDropped()));
        obj->setProperty ("audioDropped", (juce::int64) (droppedAudio + r->audioDropped()));
    }

    return juce::var (obj.release());
}

//==============================================================================
void EngineHost::reloadLater (int slot)
{
    worker.addJob ([this, slot]
    {
        PlaybackEngine* e = nullptr;
        {
            const std::scoped_lock sl (engineLock);
            e = engine.get();
        }

        if (e != nullptr)
            e->reloadSlot (slot);
    });
}

bool EngineHost::keepTake (int slot, int id)
{
    PlaybackEngine* e;
    { const std::scoped_lock sl (engineLock); e = engine.get(); }
    if (e == nullptr || ! e->history().keep (slot, id)) return false;
    reloadLater (slot);
    return true;
}

bool EngineHost::retakeTake (int slot, int id)
{
    PlaybackEngine* e;
    { const std::scoped_lock sl (engineLock); e = engine.get(); }
    if (e == nullptr || ! e->history().retake (slot, id)) return false;
    reloadLater (slot);
    return true;
}

bool EngineHost::restoreTake (int slot, int id)
{
    PlaybackEngine* e;
    { const std::scoped_lock sl (engineLock); e = engine.get(); }
    if (e == nullptr || ! e->history().restore (slot, id)) return false;
    reloadLater (slot);
    return true;
}

bool EngineHost::setSlip (int slot, int id, double seconds)
{
    PlaybackEngine* e;
    { const std::scoped_lock sl (engineLock); e = engine.get(); }
    if (e == nullptr || ! e->history().setSlip (slot, id, seconds)) return false;
    reloadLater (slot);
    return true;
}

} // namespace griddy
