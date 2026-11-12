#include "Hub.h"

#include "CollageLayout.h"

namespace griddy
{

static int readInt (const std::atomic<float>* p, int fallback)
{
    return p != nullptr ? (int) std::lround (p->load()) : fallback;
}

Hub::Hub()
    : juce::Thread ("Griddy hub"), videoEngine (unusedTransport, EngineControls {})
{
    for (int i = 0; i < maxSlots; ++i)
    {
        audioRecorders[(size_t) i] = nullptr;
        slotBusy[(size_t) i] = false;
        slotRecording[(size_t) i] = false;
        droppedFrames[(size_t) i] = 0;
        droppedAudio[(size_t) i] = 0;
    }

    // The hub draws the grid (live cameras where a slot has no take, takes where it has).
    videoEngine.setComposeEnabled (false);

    // A fresh set starts with test patterns so there is something on screen before a camera is picked.
    for (int slot = 0; slot < 4; ++slot)
        videoEngine.setSlotSource (slot, "test:" + juce::String (slot + 1));

    videoEngine.startEngine();
    sessionId = juce::Uuid().toDashedString();
    startThread (juce::Thread::Priority::normal);
}

Hub::~Hub()
{
    stopThread (5000);
    audioExporter = nullptr;
    worker.removeAllJobs (true, 60000);
    destroyEngine();
}

//==============================================================================
int Hub::numMembers() const
{
    const std::scoped_lock sl (memberLock);
    return (int) members.size();
}

int Hub::firstFreeSlot() const
{
    for (int s = 0; s < maxSlots; ++s)
    {
        bool taken = false;
        for (const auto& m : members)
            taken = taken || m->slot.load() == s;

        if (! taken)
            return s;
    }

    return -1;
}

Member* Hub::addMember (MemberControls controls)
{
    const std::scoped_lock sl (memberLock);

    auto m = std::make_unique<Member> (controls);
    const auto wanted = readInt (controls.slot, 1) - 1;

    bool free = juce::isPositiveAndBelow (wanted, maxSlots);
    for (const auto& other : members)
        free = free && other->slot.load() != wanted;

    const auto slot = free ? wanted : firstFreeSlot();
    m->slot = slot;

    // More than nine instances: the extras have no slot and just pass audio through.
    if (slot >= 0 && slot != wanted)
        m->pendingParamSlot = slot;

    auto* raw = m.get();
    members.push_back (std::move (m));
    return raw;
}

void Hub::removeMember (Member* member)
{
    int slot = -1;
    {
        const std::scoped_lock sl (memberLock);
        slot = member->slot.load();
        member->slot = -1;      // the audio thread of this instance is already stopped by the host
        member->wantRecording = false;
    }

    // A take in progress is finished properly before the instance goes.
    if (slot >= 0 && slotRecording[(size_t) slot].load())
    {
        worker.addJob ([this, slot] { finishTake (slot); });
        worker.removeAllJobs (false, 60000);
    }

    {
        const std::scoped_lock sl (memberLock);
        members.erase (std::remove_if (members.begin(), members.end(), [&] (auto& m) { return m.get() == member; }), members.end());
    }
}

void Hub::settleSlots()
{
    struct Show { Member* m; int slot; int master; };
    std::vector<Show> toShow;
    {
        const std::scoped_lock sl (memberLock);
        uint32_t mask = 0;
        int minSlot = maxSlots;
        Member* lead = nullptr;
        int cameras = 1;

        for (auto& m : members)
        {
            // Master mode: no slot, no audio, no recording. Only one instance can be the master.
            const auto wantMaster = m->controls.master != nullptr && m->controls.master->load() > 0.5f;

            if (wantMaster == m->isMaster.load())
                m->masterQueued = false;
            else if (wantMaster)
            {
                bool other = false;
                for (auto& o : members)
                    other = other || (o.get() != m.get() && o->isMaster.load());

                const auto current = m->slot.load();
                if (other || (current >= 0 && slotRecording[(size_t) current].load()))
                    m->pendingMasterParam = 0; // not now: put the switch back
                else
                {
                    m->isMaster = true;
                    m->slot = -1;
                    m->masterQueued = false;
                }
            }
            else
            {
                const auto free = firstFreeSlot();
                if (free < 0)
                    m->pendingMasterParam = 1; // no cell left to go back to
                else
                {
                    m->isMaster = false;
                    m->slot = free;
                    m->pendingParamSlot = free;
                }
            }

            const auto current = m->slot.load();
            const auto wanted = readInt (m->controls.slot, current + 1) - 1;

            if (wanted == current)
                m->paramQueued = false;

            if (! m->isMaster && current >= 0 && wanted != current && juce::isPositiveAndBelow (wanted, maxSlots))
            {
                bool taken = false;
                for (auto& o : members)
                    taken = taken || (o.get() != m.get() && o->slot.load() == wanted);

                // A recording slot cannot move under its own take.
                if (! taken && ! slotRecording[(size_t) current].load())
                    m->slot = wanted;
                else
                    m->pendingParamSlot = current; // put the parameter back
            }

            if ((m->pendingParamSlot >= 0 && ! m->paramQueued) || (m->pendingMasterParam >= 0 && ! m->masterQueued))
            {
                toShow.push_back ({ m.get(), m->pendingParamSlot, m->pendingMasterParam });
                m->paramQueued = m->paramQueued || m->pendingParamSlot >= 0;
                m->masterQueued = m->masterQueued || m->pendingMasterParam >= 0;
            }

            m->pendingParamSlot = -1;
            m->pendingMasterParam = -1;

            if (const auto s = m->slot.load(); s >= 0)
            {
                mask |= 1u << s;
                if (s < minSlot)
                {
                    minSlot = s;
                    lead = m.get();
                }

                cameras = juce::jmax (cameras, s + 1);
            }

            cameras = juce::jmax (cameras, readInt (m->controls.cameras, 1));
        }

        claimedMask = mask;
        leader = lead;
        activeSlots = juce::jlimit (1, maxSlots, cameras);
    }

    // Parameters belong to the message thread. The member may be gone by then, so look it up again.
    for (const auto& show : toShow)
        juce::MessageManager::callAsync ([this, show]
        {
            std::function<void (int)> slotFn;
            std::function<void (bool)> masterFn;
            {
                const std::scoped_lock sl (memberLock);
                for (auto& x : members)
                    if (x.get() == show.m)
                    {
                        slotFn = x->controls.setSlotParameter;
                        masterFn = x->controls.setMasterParameter;
                    }
            }

            if (show.slot >= 0 && slotFn)
                slotFn (show.slot);

            if (show.master >= 0 && masterFn)
                masterFn (show.master != 0);
        });
}

//==============================================================================
void Hub::buildEngine()
{
    PlaybackEngine::Config cfg;
    cfg.numSlots = maxSlots;
    cfg.sampleRate = sampleRate;
    cfg.canvas = Canvas::hd720;
    cfg.runRenderThread = false; // the hub draws the grid, including the live camera
    cfg.takesRoot = videoEngine.getOutputFolder().getChildFile ("Sessions").getChildFile (sessionId);
    cfg.takesRoot.createDirectory();

    auto e = std::make_unique<PlaybackEngine> (cfg);
    e->start();

    {
        const std::scoped_lock sl (engineLock);
        engine = std::move (e);

        for (int s = 0; s < maxSlots; ++s)
        {
            recorders[(size_t) s] = std::make_unique<TakeRecorder> (engine->history());
            audioRecorders[(size_t) s] = recorders[(size_t) s].get();
        }

        audioEngine = engine.get();
    }

    // Takes from earlier runs of this project: analyse them so old clips find their video.
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

void Hub::destroyEngine()
{
    // Audio threads read these pointers. Withdraw them, give blocks already running time to end, then destroy.
    audioEngine = nullptr;
    for (auto& r : audioRecorders)
        r = nullptr;

    juce::Thread::sleep (40);

    std::unique_ptr<PlaybackEngine> e;
    std::array<std::unique_ptr<TakeRecorder>, maxSlots> rs;
    {
        const std::scoped_lock sl (engineLock);
        for (size_t i = 0; i < rs.size(); ++i)
            rs[i] = std::move (recorders[i]);
        e = std::move (engine);
    }

    for (auto& r : rs)
        if (r != nullptr && r->isRecording())
            r->stop();

    if (e != nullptr)
        e->stop();
}

void Hub::prepare (double sr, int maxBlockSize)
{
    maxBlock = juce::jmax (64, maxBlockSize);

    if (sr == sampleRate && audioEngine.load() != nullptr)
        return;

    worker.removeAllJobs (true, 60000);
    destroyEngine();
    sampleRate = sr;
    buildEngine();
}

void Hub::setSession (const juce::String& id)
{
    if (id.isEmpty() || id == sessionId)
        return;

    // One project per DAW process: the first instance to name a session decides, the rest join it.
    if (sessionAdopted.exchange (true))
        return;

    worker.removeAllJobs (true, 60000);
    destroyEngine();
    sessionId = id;

    if (sampleRate > 0.0)
        buildEngine();
}

bool Hub::changeOutputFolder (const juce::File& folder)
{
    if (folder == juce::File() || folder == videoEngine.getOutputFolder())
        return true;

    for (auto& r : slotRecording)
        if (r.load())
            return false;

    const auto es = exportState.load();
    if (es == exportWaiting || es == exportRecording || es == exportWriting)
        return false;

    folder.createDirectory();
    if (! folder.isDirectory())
        return false;

    worker.removeAllJobs (true, 60000);
    destroyEngine();
    videoEngine.setOutputFolder (folder);
    if (sampleRate > 0.0)
        buildEngine();
    return true;
}

juce::String Hub::getSession() const
{
    return sessionId;
}

//==============================================================================
void Hub::processBlock (Member& member, const juce::AudioBuffer<float>& buffer, const TransportSnapshot& t) noexcept
{
    auto* e = audioEngine.load (std::memory_order_acquire);
    const auto slot = member.slot.load (std::memory_order_relaxed);

    // Arm gates everything, so a stray play never records over a take.
    const auto armed = member.controls.recordArm != nullptr && member.controls.recordArm->load() > 0.5f;
    const auto mode = readInt (member.controls.recordMode, 0);
    const auto want = slot >= 0 && armed && t.isPlaying && (mode == 0 ? t.isRecording : true);

    if (want && ! member.wantRecording.load (std::memory_order_relaxed))
    {
        member.requestSample = t.timeInSamples;
        member.requestPpq = t.ppqPosition;
        member.requestBpm = t.bpm;
        member.requestNum = t.timeSigNumerator;
        member.requestDen = t.timeSigDenominator;
    }

    member.wantRecording.store (want, std::memory_order_release);
    member.playing.store (t.isPlaying, std::memory_order_relaxed);
    member.hostRecording.store (t.isRecording, std::memory_order_relaxed);

    // The master instance hears the mix; while an export runs it hands that to the exporter.
    if (member.isMaster.load (std::memory_order_relaxed))
        if (auto* x = audioExporter.load (std::memory_order_acquire); x != nullptr && t.isPlaying && buffer.getNumChannels() > 0)
            x->pushAudio (buffer.getReadPointer (0), buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : nullptr, buffer.getNumSamples());

    if (e == nullptr || slot < 0)
        return;

    static const float silence[512] = {};
    float mono[512];

    auto* recorder = audioRecorders[(size_t) slot].load (std::memory_order_acquire);
    const auto isLeader = leader.load (std::memory_order_relaxed) == &member;
    const auto claimed = claimedMask.load (std::memory_order_relaxed);
    const auto active = activeSlots.load (std::memory_order_relaxed);

    const auto channels = buffer.getNumChannels();
    const auto total = buffer.getNumSamples();

    for (int done = 0; done < total;)
    {
        const auto n = juce::jmin (total - done, 512);
        const auto host = t.timeInSamples + done;

        if (channels <= 0)
            std::fill (mono, mono + n, 0.0f);
        else
        {
            const auto* c0 = buffer.getReadPointer (0) + done;
            if (channels == 1)
                std::copy (c0, c0 + n, mono);
            else
            {
                const auto* c1 = buffer.getReadPointer (1) + done;
                for (int i = 0; i < n; ++i)
                    mono[i] = 0.5f * (c0[i] + c1[i]);
            }
        }

        e->feedSlotAudio (slot, mono, n, host, t.isPlaying);

        if (want && recorder != nullptr && recorder->isRecording())
            recorder->feedAudio (mono, n, host);

        // Slots nobody owns follow the host position. One writer per slot, so only the leader does this.
        if (isLeader)
            for (int s = 0; s < active; ++s)
                if ((claimed & (1u << s)) == 0)
                    e->feedSlotAudio (s, silence, n, host, t.isPlaying);

        done += n;
    }
}

//==============================================================================
void Hub::run()
{
    while (! threadShouldExit())
    {
        controlTick();
        wait (4);
    }
}

void Hub::controlTick()
{
    PlaybackEngine* e = nullptr;
    {
        const std::scoped_lock sl (engineLock);
        e = engine.get();
    }

    settleSlots();

    if (e != nullptr)
    {
        const std::scoped_lock sl (memberLock);

        for (auto& mp : members)
        {
            auto& m = *mp;
            const auto slot = m.slot.load();
            if (slot < 0)
                continue;

            TakeRecorder* r = nullptr;
            {
                const std::scoped_lock el (engineLock);
                r = recorders[(size_t) slot].get();
            }

            if (r == nullptr)
                continue;

            const auto want = m.wantRecording.load (std::memory_order_acquire);

            if (want && ! m.recording && ! slotBusy[(size_t) slot].load())
            {
                TakeMeta meta;
                meta.sampleRate = sampleRate;
                meta.startHostSample = m.requestSample.load();
                meta.startPpq = m.requestPpq.load();
                meta.bpm = m.requestBpm.load();
                meta.timeSigNumerator = m.requestNum.load();
                meta.timeSigDenominator = m.requestDen.load();

                TakeRecorder::Config cfg;
                cfg.embedSync = false;

                if (r->start (slot, meta, cfg))
                {
                    m.recording = true;
                    m.lastCameraSerial = 0;
                    slotRecording[(size_t) slot] = true;
                }
            }
            else if (! want && m.recording && ! slotBusy[(size_t) slot].load())
            {
                m.recording = false;
                slotBusy[(size_t) slot] = true;

                // Stopping writes the last files and take.json; rebuilding the slot analyses its audio.
                worker.addJob ([this, slot]
                {
                    finishTake (slot);
                    slotBusy[(size_t) slot] = false;
                });
            }

            if (m.recording)
                if (auto frame = videoEngine.latestFrame (slot); frame != nullptr && (uint64_t) frame->serial != m.lastCameraSerial)
                {
                    m.lastCameraSerial = (uint64_t) frame->serial;
                    r->pushFrame (frame->image, frame->captureTimeSeconds);
                }
        }
    }

    updateExport();

    if ((tick++ % 8) == 0)
        drawPreview();
}

//==============================================================================
bool Hub::startExport()
{
    if (! gridExportAvailable())
    {
        const std::scoped_lock sl (statusLock);
        exportError = "This build cannot write MP4 files.";
        exportState = exportFailed;
        return false;
    }

    auto s = exportState.load();
    if (s == exportWaiting || s == exportRecording || s == exportWriting)
        return false;

    {
        const std::scoped_lock sl (statusLock);
        exportError.clear();
    }

    exportStopRequested = false;
    exportState = exportWaiting;
    return true;
}

void Hub::stopExport()
{
    if (exportState.load() == exportWaiting)
        exportState = exportIdle;
    else
        exportStopRequested = true;
}

void Hub::updateExport()
{
    const auto state = exportState.load();
    if (state != exportWaiting && state != exportRecording)
        return;

    bool masterPlaying = false;
    {
        const std::scoped_lock sl (memberLock);
        for (const auto& m : members)
            if (m->isMaster.load())
                masterPlaying = masterPlaying || m->playing.load();
    }

    if (state == exportWaiting && masterPlaying)
    {
        const auto look = getLayout();
        const auto size = canvasSize (look.canvas);

        ExportSettings cfg;
        cfg.width = size.x;
        cfg.height = size.y;
        cfg.fps = 30;
        cfg.sampleRate = sampleRate;
        cfg.file = videoEngine.getOutputFolder().getChildFile ("Exports")
                       .getChildFile (juce::File::createLegalFileName (sessionId + " " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S")) + ".mp4");

        juce::String error;
        if (exporter.start (cfg, error))
        {
            {
                const std::scoped_lock sl (statusLock);
                exportFile = cfg.file;
            }

            audioExporter = &exporter;
            exportState = exportRecording;
        }
        else
        {
            const std::scoped_lock sl (statusLock);
            exportError = error;
            exportState = exportFailed;
        }
    }
    else if (state == exportRecording && (! masterPlaying || exportStopRequested.load()))
    {
        audioExporter = nullptr;
        exportState = exportWriting;

        worker.addJob ([this]
        {
            juce::Thread::sleep (30); // a block already running on the audio thread finishes first
            juce::String error;
            const auto ok = exporter.finish (error);
            {
                const std::scoped_lock sl (statusLock);
                exportError = ok ? juce::String() : error;
            }
            exportState = ok ? exportDone : exportFailed;
        });
    }
}

void Hub::finishTake (int slot)
{
    PlaybackEngine* e = nullptr;
    TakeRecorder* r = nullptr;
    {
        const std::scoped_lock sl (engineLock);
        e = engine.get();
        r = recorders[(size_t) slot].get();
    }

    if (e == nullptr || r == nullptr)
    {
        slotRecording[(size_t) slot] = false;
        return;
    }

    if (const auto entry = r->stop())
    {
        droppedFrames[(size_t) slot] += r->framesDropped();
        droppedAudio[(size_t) slot] += r->audioDropped();
        slotRecording[(size_t) slot] = false;
        e->reloadSlot (slot);
    }
    else
    {
        slotRecording[(size_t) slot] = false;
        const std::scoped_lock sl (statusLock);
        lastError = "A take could not be finished.";
    }
}

GridLayout Hub::getLayout() const
{
    const std::scoped_lock sl (layoutLock);
    return layout;
}

void Hub::setLayout (Canvas canvas, float gap, uint32_t background, Template shape)
{
    const std::scoped_lock sl (layoutLock);
    // The file keeps the size it started with, so the canvas stays put while an export runs.
    const auto es = exportState.load();
    if (es != exportRecording && es != exportWriting)
        layout.canvas = canvas;
    layout.shape = shape;
    layout.gap = juce::jlimit (0.0f, 48.0f, gap);
    layout.background = background | 0xff000000u;
}

bool Hub::swapCells (int a, int b)
{
    const auto active = activeSlots.load();

    if (! juce::isPositiveAndBelow (a, active) || ! juce::isPositiveAndBelow (b, active))
        return false;

    const std::scoped_lock sl (layoutLock);
    std::swap (layout.order[(size_t) a], layout.order[(size_t) b]);
    return true;
}

juce::var Hub::layoutToVar() const
{
    const auto l = getLayout();
    auto o = std::make_unique<juce::DynamicObject>();
    o->setProperty ("canvas", (int) l.canvas);
    o->setProperty ("template", (int) l.shape);
    o->setProperty ("gap", (double) l.gap);
    o->setProperty ("background", juce::String::toHexString ((int) (l.background & 0xffffffu)).paddedLeft ('0', 6));

    juce::Array<juce::var> order;
    for (auto v : l.order)
        order.add (v);
    o->setProperty ("order", order);
    return juce::var (o.release());
}

void Hub::applyLayoutVar (const juce::var& v)
{
    if (! v.isObject())
        return;

    const auto canvas = juce::jlimit (0, 3, (int) v.getProperty ("canvas", 0));
    const auto gap = (float) (double) v.getProperty ("gap", 4.0);
    const auto bg = (uint32_t) juce::String (v.getProperty ("background", "0b0b0f")).getHexValue32();
    setLayout ((Canvas) canvas, gap, bg, (Template) juce::jlimit (0, numTemplates - 1, (int) v.getProperty ("template", 0)));

    if (const auto* order = v.getProperty ("order", {}).getArray(); order != nullptr && order->size() == 9)
    {
        std::array<int, 9> parsed {};
        uint32_t seen = 0;
        bool ok = true;

        for (int i = 0; i < 9; ++i)
        {
            parsed[(size_t) i] = (int) (*order)[i];
            ok = ok && juce::isPositiveAndBelow (parsed[(size_t) i], 9) && (seen & (1u << parsed[(size_t) i])) == 0;
            seen |= 1u << juce::jlimit (0, 8, parsed[(size_t) i]);
        }

        if (ok)
        {
            const std::scoped_lock sl (layoutLock);
            layout.order = parsed;
        }
    }
}

void Hub::drawPreview()
{
    PlaybackEngine* e = nullptr;
    {
        const std::scoped_lock sl (engineLock);
        e = engine.get();
    }

    if (e == nullptr)
        return;

    const auto active = activeSlots.load();
    const auto look = getLayout();

    // The canvas can change under us (aspect ratio): redraw into images of the right shape.
    const auto size = canvasSize (look.canvas);
    if (output.getWidth() != size.x || output.getHeight() != size.y)
    {
        output = juce::Image (juce::Image::ARGB, size.x, size.y, true, juce::SoftwareImageType());
        previewImage = juce::Image (juce::Image::ARGB, VideoEngine::previewWidth,
                                    juce::jmax (16, (int) std::lround ((double) VideoEngine::previewWidth * size.y / size.x)),
                                    true, juce::SoftwareImageType());
    }

    compositor.setGapPixels (look.gap * (float) size.x / 1280.0f);
    compositor.setBackground (look.background);

    // The cell order is only honoured if it is a real permutation of the active slots.
    auto cells = computeLayout (active, look.shape);
    {
        uint32_t seen = 0;
        bool valid = true;
        for (int i = 0; i < active; ++i)
        {
            valid = valid && juce::isPositiveAndBelow (look.order[(size_t) i], active) && (seen & (1u << look.order[(size_t) i])) == 0;
            seen |= 1u << juce::jlimit (0, 8, look.order[(size_t) i]);
        }

        if (valid)
            for (auto& c : cells)
                c.slot = look.order[(size_t) c.slot];
    }

    compositor.render (output, cells, [&] (int slot) -> std::shared_ptr<const VideoFrame>
    {
        // A slot being recorded shows its live camera. Others show their take, or the live camera if they have none.
        if (slotRecording[(size_t) slot].load())
            return videoEngine.latestFrame (slot);

        if (const auto shown = e->shown (slot); shown != nullptr && shown->frame != nullptr)
            return shown->frame;

        return videoEngine.latestFrame (slot);
    });

    if (exportState.load() == exportRecording)
        exporter.pushVideo (output, exporter.clockSeconds());

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

juce::MemoryBlock Hub::getPreviewJpeg() const
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
        case SyncKind::embedded:     return "embedded";
        case SyncKind::fingerprint:  return "fingerprint";
        case SyncKind::hostPosition: return "hostPosition";
        default: break;
    }
    return "none";
}

juce::var Hub::takesToVar (int slot) const
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

juce::var Hub::getStatus (const Member* member) const
{
    const auto mySlot = member != nullptr ? member->slot.load() : -1;

    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty ("session", sessionId);
    obj->setProperty ("slot", mySlot);
    obj->setProperty ("targetSlot", mySlot);
    obj->setProperty ("instances", numMembers());
    obj->setProperty ("recording", mySlot >= 0 && slotRecording[(size_t) mySlot].load());
    obj->setProperty ("armed", member != nullptr && member->controls.recordArm != nullptr && member->controls.recordArm->load() > 0.5f);
    obj->setProperty ("sampleRate", sampleRate);

    // Say why an armed instance is not recording, so a silent failure is never a mystery.
    juce::String hint;
    if (member != nullptr && mySlot >= 0 && member->controls.recordArm != nullptr && member->controls.recordArm->load() > 0.5f
        && ! slotRecording[(size_t) mySlot].load())
    {
        const auto mode = readInt (member->controls.recordMode, 0);
        if (! member->playing.load())
            hint = "Armed. Press play in your DAW to start a take.";
        else if (mode == 0 && ! member->hostRecording.load())
            hint = "Armed and playing, but the DAW is not sending its record flag. Switch Record when to DAW plays.";
        else if (audioEngine.load() == nullptr)
            hint = "Griddy's engine is not running yet.";
        else
            hint = "Starting the take...";
    }
    obj->setProperty ("recordHint", hint);
    obj->setProperty ("hostRecording", member != nullptr && member->hostRecording.load());
    obj->setProperty ("claimedMask", (int) claimedMask.load());
    obj->setProperty ("master", member != nullptr && member->isMaster.load());
    obj->setProperty ("layout", layoutToVar());

    {
        const std::scoped_lock sl (statusLock);
        obj->setProperty ("error", lastError);

        auto x = std::make_unique<juce::DynamicObject>();
        static const char* names[] = { "idle", "waiting", "recording", "writing", "done", "failed" };
        x->setProperty ("available", gridExportAvailable());
        x->setProperty ("state", names[juce::jlimit (0, 5, exportState.load())]);
        x->setProperty ("seconds", exportState.load() == exportRecording ? exporter.clockSeconds() : 0.0);
        x->setProperty ("file", exportFile.getFullPathName());
        x->setProperty ("error", exportError);
        obj->setProperty ("export", juce::var (x.release()));
    }

    const auto active = activeSlots.load();

    juce::Array<juce::var> slotList;
    {
        const std::scoped_lock sl (engineLock);
        if (engine != nullptr)
            for (int s = 0; s < active; ++s)
            {
                auto o = std::make_unique<juce::DynamicObject>();
                const auto shown = engine->shown (s);
                o->setProperty ("slot", s);
                o->setProperty ("claimed", (claimedMask.load() & (1u << s)) != 0);
                o->setProperty ("recording", slotRecording[(size_t) s].load());
                o->setProperty ("status", shown != nullptr ? statusName (shown->status) : "noTake");
                o->setProperty ("source", shown != nullptr ? kindName (shown->kind) : "none");
                o->setProperty ("takeId", shown != nullptr ? shown->takeId : -1);
                o->setProperty ("warp", engine->slotLooksWarped (s));
                o->setProperty ("lockMs", engine->lockLatencySeconds (s) * 1000.0);
                slotList.add (juce::var (o.release()));
            }
    }
    obj->setProperty ("slots", slotList);

    juce::Array<juce::var> takes;
    for (int s = 0; s < active; ++s)
        takes.add (takesToVar (s));
    obj->setProperty ("takes", takes);

    if (mySlot >= 0)
        if (auto* r = audioRecorders[(size_t) mySlot].load())
        {
            obj->setProperty ("framesWritten", (juce::int64) r->framesWritten());
            obj->setProperty ("framesDropped", (juce::int64) (droppedFrames[(size_t) mySlot] + r->framesDropped()));
            obj->setProperty ("audioDropped", (juce::int64) (droppedAudio[(size_t) mySlot] + r->audioDropped()));
        }

    return juce::var (obj.release());
}

//==============================================================================
void Hub::reloadLater (int slot)
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

#define GRIDDY_HUB_TAKE_ACTION(name, call)                      \
    bool Hub::name                                              \
    {                                                           \
        PlaybackEngine* e;                                      \
        { const std::scoped_lock sl (engineLock); e = engine.get(); } \
        if (e == nullptr || ! juce::isPositiveAndBelow (slot, maxSlots) || ! e->history().call) \
            return false;                                       \
        reloadLater (slot);                                     \
        return true;                                            \
    }

GRIDDY_HUB_TAKE_ACTION (keepTake (int slot, int id), keep (slot, id))
GRIDDY_HUB_TAKE_ACTION (retakeTake (int slot, int id), retake (slot, id))
GRIDDY_HUB_TAKE_ACTION (restoreTake (int slot, int id), restore (slot, id))
GRIDDY_HUB_TAKE_ACTION (setSlip (int slot, int id, double seconds), setSlip (slot, id, seconds))

#undef GRIDDY_HUB_TAKE_ACTION

} // namespace griddy
