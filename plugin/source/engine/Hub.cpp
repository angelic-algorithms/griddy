#include "Hub.h"

#include "CollageLayout.h"

#include <set>

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
        resetRequested[(size_t) i] = false;
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
    cleaner.removeAllJobs (true, 60000);
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
    // Every instance of the last set is gone, so this one belongs to a new set: it starts with an empty session.
    if (freshOnNextMember)
    {
        freshOnNextMember = false;
        originId.clear();
        switchSession (juce::Uuid().toDashedString(), "a new set (every instance of the last one had closed)");
    }

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

    // If the instances of the project whose session is open are all gone while instances of another project remain
    // (a host that loads the next set before it closes the last), that other project's session opens now.
    juce::String nextId;
    std::optional<std::vector<int32_t>> nextTakes;
    {
        const std::scoped_lock sl (memberLock);
        members.erase (std::remove_if (members.begin(), members.end(), [&] (auto& m) { return m.get() == member; }), members.end());

        if (members.empty())
            freshOnNextMember = true;
        else if (originId.isNotEmpty())
        {
            // Only when every instance left names one other project: a new instance (no saved state) keeps the session.
            bool allOther = true;
            for (auto& m : members)
                allOther = allOther && m->stateSession.isNotEmpty() && m->stateSession != originId
                                    && m->stateSession == members.front()->stateSession;

            if (allOther)
            {
                nextId = members.front()->stateSession;
                nextTakes = members.front()->stateTakes;
            }
        }
    }

    if (nextId.isNotEmpty())
        openSaved (nextId, nextTakes);
}

void Hub::settleSlots()
{
    struct Show { Member* m; int slot; int master; };
    std::vector<Show> toShow;
    {
        const std::scoped_lock sl (memberLock);
        uint32_t mask = 0, armed = 0;
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

            // While the hub is still showing a slot it chose in the parameter, the parameter is not a request.
            if (! m->isMaster && current >= 0 && wanted != current && juce::isPositiveAndBelow (wanted, maxSlots)
                && m->pendingParamSlot < 0 && ! m->paramQueued)
            {
                Member* holder = nullptr;
                for (auto& o : members)
                    if (o.get() != m.get() && o->slot.load() == wanted)
                        holder = o.get();

                // A slot taken by another track swaps: that track moves to this one's old slot. Takes stay with
                // their slots. Neither slot may be recording, so no take moves under its own recording.
                if (slotRecording[(size_t) current].load() || slotRecording[(size_t) wanted].load())
                {
                    m->pendingParamSlot = current; // put the parameter back
                }
                else
                {
                    if (holder != nullptr)
                    {
                        holder->slot = current;
                        holder->pendingParamSlot = current;
                        log ("slot " + juce::String (wanted + 1) + " and slot " + juce::String (current + 1) + " swap tracks");
                    }
                    m->slot = wanted;
                }
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
                if (! m->isMaster && m->controls.recordArm != nullptr && m->controls.recordArm->load() > 0.5f)
                    armed |= 1u << s;
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
        armedMask = armed;
        leader = lead;
        activeSlots = juce::jlimit (1, maxSlots, cameras);
    }

    // Parameters belong to the message thread. The member, or the whole hub, may be gone by then, so look it up again.
    for (const auto& show : toShow)
        juce::MessageManager::callAsync ([self = juce::WeakReference<Hub> (this), show]
        {
            if (self == nullptr)
                return;

            auto* hub = self.get();
            std::function<void (int)> slotFn;
            std::function<void (bool)> masterFn;
            {
                const std::scoped_lock sl (hub->memberLock);
                for (auto& x : hub->members)
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
    cfg.takesRoot = getSessionFolder();
    cfg.takesRoot.createDirectory();
    log ("engine ready: session " + getSession() + " at " + juce::String (sampleRate, 0) + " Hz");

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

        purgeLater(); // anything discarded before the DAW closed last time
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

void Hub::loadSession (Member* member, const juce::String& savedId, const std::optional<std::vector<int32_t>>& savedTakes)
{
    if (savedId.isEmpty())
        return;

    if (member != nullptr)
    {
        member->stateSession = savedId;
        member->stateTakes = savedTakes;
    }

    // The project's other instances name the same session as the one that decided.
    if (savedId == originId || savedId == getSession())
        return;

    // Another instance of the open project has already loaded: this device came from somewhere else (pasted, or dragged
    // in from another set). It joins the open session; it never switches the grid under the takes already there.
    {
        const std::scoped_lock sl (memberLock);
        for (auto& m : members)
            if (m.get() != member && m->stateSession.isNotEmpty() && (m->stateSession == originId || m->stateSession == getSession()))
            {
                log ("an instance from session " + savedId + " joins the open session " + getSession());
                return;
            }
    }

    // A fresh session that already has takes of its own stays; those takes were recorded in this set.
    if (originId.isEmpty() && ! session::takeIdsOnDisk (getSessionFolder()).empty())
    {
        log ("an instance from session " + savedId + " joins the open session " + getSession() + ", which has takes of its own");
        return;
    }

    openSaved (savedId, savedTakes);
}

void Hub::openSaved (const juce::String& savedId, const std::optional<std::vector<int32_t>>& savedTakes)
{
    const auto choice = session::choose (videoEngine.getOutputFolder().getChildFile ("Sessions"), savedId, savedTakes);
    originId = savedId;

    juce::String why = choice.kind == SessionChoice::Kind::reopened ? "reopened saved session " + savedId
                     : choice.kind == SessionChoice::Kind::forked ? "new session for saved session " + savedId + " with its "
                                                                     + juce::String (choice.linkedTakes) + " takes"
                                                                   : "new empty session instead of saved session " + savedId;
    switchSession (choice.id, why + ": " + choice.reason);
}

void Hub::switchSession (const juce::String& id, const juce::String& why)
{
    log ("session " + id + ": " + why);

    if (id == getSession())
        return;

    worker.removeAllJobs (true, 60000);
    destroyEngine();
    {
        const std::scoped_lock sl (sessionLock);
        sessionId = id;
    }

    for (auto& r : resetRequested)
        r = false;

    if (sampleRate > 0.0)
        buildEngine();
}

bool Hub::startNewSession()
{
    for (auto& r : slotRecording)
        if (r.load())
            return false;

    const auto es = exportState.load();
    if (es == exportWaiting || es == exportRecording || es == exportWriting)
        return false;

    const auto id = juce::Uuid().toDashedString();
    originId = id;
    {
        const std::scoped_lock sl (memberLock);
        for (auto& m : members)
        {
            m->stateSession = id;
            m->stateTakes.reset();
        }
    }

    switchSession (id, "New session pressed; the previous session " + getSession() + " stays on disk");
    return true;
}

bool Hub::resetSlot (int slot)
{
    if (! juce::isPositiveAndBelow (slot, maxSlots))
        return false;

    // Arm off first, so the take in progress ends and nothing starts again.
    std::function<void (bool)> armFn;
    {
        const std::scoped_lock sl (memberLock);
        for (auto& m : members)
            if (m->slot.load() == slot)
                armFn = m->controls.setArmParameter;
    }

    if (armFn)
        armFn (false);

    log ("slot " + juce::String (slot + 1) + ": Reset slot pressed");
    resetRequested[(size_t) slot] = true;
    return true;
}

void Hub::processResets (PlaybackEngine* e)
{
    // Control thread. A reset waits until the slot's take has ended and been written, then archives its takes.
    for (int s = 0; s < maxSlots; ++s)
    {
        if (! resetRequested[(size_t) s].load() || slotRecording[(size_t) s].load() || slotBusy[(size_t) s].load())
            continue;

        bool recordingHere = false;
        {
            const std::scoped_lock sl (memberLock);
            for (auto& m : members)
                recordingHere = recordingHere || (m->slot.load() == s && m->recording);
        }

        if (recordingHere || e == nullptr)
            continue;

        resetRequested[(size_t) s] = false;
        slotBusy[(size_t) s] = true;
        worker.addJob ([this, s]
        {
            PlaybackEngine* engineNow = nullptr;
            {
                const std::scoped_lock sl (engineLock);
                engineNow = engine.get();
            }

            if (engineNow != nullptr)
            {
                const auto n = engineNow->history().archiveAll (s);
                engineNow->reloadSlot (s);
                log ("slot " + juce::String (s + 1) + ": reset, " + juce::String (n) + " takes archived, the slot shows its camera");
            }

            slotBusy[(size_t) s] = false;
        });
    }
}

bool Hub::changeOutputFolder (const juce::File& folder)
{
    if (folder == juce::File())
        return true;

    {
        const std::scoped_lock sl (statusLock);
        pendingFolder = juce::File();
    }

    if (applyOutputFolder (folder))
        return true;

    const std::scoped_lock sl (statusLock);
    pendingFolder = folder;
    return false;
}

bool Hub::applyOutputFolder (const juce::File& folder)
{
    if (folder == videoEngine.getOutputFolder())
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
    const std::scoped_lock sl (sessionLock);
    return sessionId;
}

juce::File Hub::getSessionFolder() const
{
    return videoEngine.getOutputFolder().getChildFile ("Sessions").getChildFile (getSession());
}

std::vector<int32_t> Hub::sessionTakes() const
{
    return session::takeIdsOnDisk (getSessionFolder());
}

//==============================================================================
void Hub::processBlock (Member& member, const juce::AudioBuffer<float>& buffer, const TransportSnapshot& t) noexcept
{
    auto* e = audioEngine.load (std::memory_order_acquire);
    const auto slot = member.slot.load (std::memory_order_relaxed);

    // Arm gates everything, so a stray play never records over a take.
    const auto armed = member.controls.recordArm != nullptr && member.controls.recordArm->load() > 0.5f;
    if (! armed)
        member.takeDone.store (false, std::memory_order_relaxed);

    // The one rule: armed and playing records one take. Live sends no record flag, so its record button cannot be the gate.
    const auto want = slot >= 0 && armed && t.isPlaying && ! member.takeDone.load (std::memory_order_relaxed);

    if (want && ! member.wantRecording.load (std::memory_order_relaxed))
    {
        member.requestSample = t.timeInSamples;
        member.requestPpq = t.ppqPosition;
        member.requestBpm = t.bpm;
        member.requestNum = t.timeSigNumerator;
        member.requestDen = t.timeSigDenominator;
    }

    member.hostSample.store (t.timeInSamples, std::memory_order_relaxed);
    member.hostPpq.store (t.ppqPosition, std::memory_order_relaxed);
    member.hostLooping.store (t.isLooping, std::memory_order_relaxed);

    {
        // A meter's peak: jumps up at once, falls by half in about a fifth of a second.
        float peak = 0.0f;
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, buffer.getNumSamples()));
        const auto fall = (float) std::exp (-3.5 * buffer.getNumSamples() / juce::jmax (1.0, sampleRate));
        member.level.store (juce::jmax (peak, member.level.load (std::memory_order_relaxed) * fall), std::memory_order_relaxed);
    }
    member.wantRecording.store (want, std::memory_order_release);
    member.playing.store (t.isPlaying, std::memory_order_relaxed);
    member.hostRecording.store (t.isRecording, std::memory_order_relaxed);
    if (t.isRecording)
        member.hostRecordSeen.store (true, std::memory_order_relaxed);

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

void Hub::log (const juce::String& line)
{
    // The control thread, the worker and the message thread all write here.
    const std::scoped_lock sl (logLock);

    // One file per takes folder, started fresh when it grows past 2 MB.
    const auto folder = videoEngine.getOutputFolder().getChildFile ("Logs");
    if (logFile == nullptr || folder != logFolder)
    {
        logFolder = folder;
        folder.createDirectory();
        const auto f = folder.getChildFile ("griddy.log");
        if (f.getSize() > 2 * 1024 * 1024)
            f.deleteFile();
        logFile = std::make_unique<juce::FileOutputStream> (f);
        if (! logFile->openedOk())
        {
            logFile.reset();
            return;
        }
    }

    logFile->writeText (juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S ") + line + "\n", false, false, nullptr);
    logFile->flush();
}

void Hub::logTransport (Member& m, PlaybackEngine* e)
{
    // What the DAW really sends, written when it changes, so a session in the DAW can be read back afterwards.
    auto& l = m.logged;
    const auto playing = m.playing.load(), recording = m.hostRecording.load(), looping = m.hostLooping.load();
    const auto armed = m.controls.recordArm != nullptr && m.controls.recordArm->load() > 0.5f;
    const auto sample = m.hostSample.load();
    const auto now = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    const auto who = "instance slot " + juce::String (m.slot.load() + 1) + ": ";
    const auto where = " at sample " + juce::String (sample) + ", ppq " + juce::String (m.hostPpq.load(), 3);

    // What the slot's picture is doing: which take and frame it shows, and how sure the sync is.
    const auto slot = m.slot.load();
    std::shared_ptr<const ShownFrame> shown;
    if (e != nullptr && slot >= 0 && ! slotRecording[(size_t) slot].load())
        shown = e->shown (slot);

    const auto picture = [&]
    {
        if (slot < 0)
            return juce::String();
        if (slotRecording[(size_t) slot].load())
            return juce::String (", shows its live camera (recording)");
        if (shown == nullptr || shown->frame == nullptr)
            return juce::String (", shows its live camera (no take)");
        static const char* names[] = { "no take", "stopped", "following the playhead", "locked", "holding", "lost" };
        return ", shows take " + juce::String (shown->takeId) + " frame " + juce::String (shown->frameIndex) + " ("
               + names[juce::jlimit (0, 5, (int) shown->status)] + ")";
    };

    // Whether Live sends its record flag at all is the open question, so log every sign of it, however short.
    if (m.hostRecordSeen.exchange (false) && ! recording && ! l.recording)
        log (who + "the DAW's record flag was on for less than a control tick" + where);

    if (! l.valid || playing != l.playing || recording != l.recording || looping != l.looping || armed != l.armed)
    {
        log (who + (playing ? "playing" : "stopped") + (recording ? ", DAW recording" : ", DAW not recording") + (looping ? ", loop on" : "")
             + (armed ? ", armed" : "") + where + picture());
        l = { true, playing, recording, looping, armed, sample, now, false,
              shown != nullptr ? shown->takeId : -1, shown != nullptr ? shown->frameIndex : -1, now, false };
        return;
    }

    // Playing with a take on screen, the picture should move. Say so once if it stays on one frame for a second.
    if (shown != nullptr && shown->frame != nullptr)
    {
        if (shown->takeId != l.shownTake || shown->frameIndex != l.shownFrame)
        {
            l.shownTake = shown->takeId;
            l.shownFrame = shown->frameIndex;
            l.shownSince = now;
            l.pictureStuck = false;
        }
        else if (playing && ! l.pictureStuck && now - l.shownSince > 1.0 && shown->status != SyncStatus::held)
        {
            log (who + "playing, but the picture has stayed on one frame for a second" + where + picture());
            l.pictureStuck = true;
        }
    }

    if (! playing)
        return;

    // While playing, the position should move about as fast as the clock. A jump back is a loop wrap or a seek.
    const auto dt = now - l.wall;
    if (dt < 0.5)
        return;

    if (sample < l.sample)
        log (who + "position jumped back" + where);
    else if (sample == l.sample && ! l.stuck)
        log (who + "playing but the position is not moving" + where);

    l.stuck = sample == l.sample;
    l.sample = sample;
    l.wall = now;
}

void Hub::logTakeChanges (PlaybackEngine& e, int slot)
{
    auto takeName = [] (int32_t take, int16_t pass)
    {
        if (take < 0)
            return juce::String ("nothing");
        return "take " + juce::String (take) + (pass >= 0 ? ", pass " + juce::String (pass + 1) : juce::String (", moved"));
    };

    TakeChange c;
    while (e.popTakeChange (slot, c))
    {
        const auto where = " at host " + juce::String (c.hostSeconds, 3) + " s (" + TakeChange::pathName (c.path) + ")";

        if (c.path == TakeChange::gapStart)
            log ("slot " + juce::String (slot + 1) + ": gap, black, was " + takeName (c.oldTake, c.oldPass) + where);
        else if (c.path == TakeChange::gapEnd)
            log ("slot " + juce::String (slot + 1) + ": gap ends, " + takeName (c.newTake, c.newPass) + where);
        else
            log ("slot " + juce::String (slot + 1) + ": " + takeName (c.oldTake, c.oldPass) + " -> " + takeName (c.newTake, c.newPass) + where);
    }
}

void Hub::purgeLater()
{
    juce::File root;
    {
        const std::scoped_lock sl (engineLock);
        if (engine == nullptr)
            return;
        root = engine->history().root();
    }

    cleaner.addJob ([this, root]
    {
        TakeHistory::purgeDiscarded (root);

        // What the session's takes use on disk, for the Setup tab. Walking every frame file is slow, so it is done here.
        int64_t bytes = 0;
        for (const auto& f : root.findChildFiles (juce::File::findFiles, true))
            bytes += f.getSize();
        takesBytes = bytes;
    });
}

void Hub::disarmLater (Member* member)
{
    // Parameters belong to the message thread. The member, or the whole hub, may be gone by then, so look it up again.
    juce::MessageManager::callAsync ([self = juce::WeakReference<Hub> (this), member]
    {
        if (self == nullptr)
            return;

        std::function<void (bool)> armFn;
        {
            const std::scoped_lock sl (self->memberLock);
            for (auto& x : self->members)
                if (x.get() == member)
                    armFn = x->controls.setArmParameter;
        }

        if (armFn)
            armFn (false);
    });
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

    // A folder chosen during a take moves over once nothing is recording, before the next take can start.
    juce::File pending;
    {
        const std::scoped_lock sl (statusLock);
        pending = pendingFolder;
    }

    if (pending != juce::File() && applyOutputFolder (pending))
    {
        {
            const std::scoped_lock sl (statusLock);
            if (pendingFolder == pending)
                pendingFolder = juce::File();
        }

        const std::scoped_lock sl (engineLock);
        e = engine.get();
    }

    if (e != nullptr)
    {
        const std::scoped_lock sl (memberLock);

        for (auto& mp : members)
        {
            auto& m = *mp;
            logTransport (m, e);

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
                meta.trackName = m.trackName; // the track this take listens to; it follows edits on that track

                TakeRecorder::Config cfg;
                cfg.embedSync = false;
                cfg.flatFolder = videoEngine.getOutputFolder().getChildFile ("Takes");

                if (r->start (slot, meta, cfg))
                {
                    log ("slot " + juce::String (slot + 1) + ": take " + juce::String (r->currentTakeId()) + " starts at sample "
                         + juce::String (meta.startHostSample));
                    m.recording = true;
                    m.lastCameraSerial = 0;
                    recordStartPpq[(size_t) slot] = meta.startPpq;
                    slotRecording[(size_t) slot] = true;
                }
            }
            else if (! want && m.recording && ! slotBusy[(size_t) slot].load())
            {
                m.recording = false;
                slotBusy[(size_t) slot] = true;

                // Every play while armed would record, so the take ends the arm.
                m.takeDone = true;
                disarmLater (&m);

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

    if (e != nullptr)
    {
        // Black gaps need the slot's own track audio, so only slots a Griddy owns get them, and never while armed or recording.
        const auto claimed = claimedMask.load();
        const auto armed = armedMask.load();
        bool anyRecording = false;

        for (int s = 0; s < e->numSlots(); ++s)
        {
            const auto rec = slotRecording[(size_t) s].load();
            anyRecording = anyRecording || rec;
            e->setGapsAllowed (s, ((claimed >> s) & 1u) != 0 && ((armed >> s) & 1u) == 0 && ! rec);
            logTakeChanges (*e, s);
        }

        e->setRecordingActive (anyRecording);
    }

    processResets (e);
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
                       .getChildFile (juce::File::createLegalFileName (getSession() + " " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S")) + ".mp4");

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

    const auto t0 = juce::Time::getMillisecondCounterHiRes();

    if (const auto entry = r->stop())
    {
        const auto t1 = juce::Time::getMillisecondCounterHiRes();
        const auto& ft = entry->meta.frameTimes;
        log ("slot " + juce::String (slot + 1) + ": take " + juce::String (entry->meta.takeId) + " ends, "
             + juce::String (entry->meta.durationSeconds, 2) + " s of audio, " + juce::String (entry->meta.numFrames) + " frames from "
             + juce::String (ft.empty() ? 0.0 : ft.front(), 2) + " to " + juce::String (ft.empty() ? 0.0 : ft.back(), 2) + " s, "
             + juce::String ((juce::int64) r->framesDropped()) + " dropped");
        droppedFrames[(size_t) slot] += r->framesDropped();
        droppedAudio[(size_t) slot] += r->audioDropped();
        slotRecording[(size_t) slot] = false;
        e->reloadSlot (slot);
        log ("slot " + juce::String (slot + 1) + ": take " + juce::String (entry->meta.takeId) + " playable "
             + juce::String ((juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, 2) + " s after the stop was handled (finishing "
             + juce::String ((t1 - t0) / 1000.0, 2) + " s, loading " + juce::String ((juce::Time::getMillisecondCounterHiRes() - t1) / 1000.0, 2) + " s)");
        purgeLater();
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

void Hub::setLayout (Canvas canvas, float gap, uint32_t background, float radius)
{
    const std::scoped_lock sl (layoutLock);
    // The file keeps the size it started with, so the canvas stays put while an export runs.
    const auto es = exportState.load();
    if (es != exportRecording && es != exportWriting)
        layout.canvas = canvas;
    layout.gap = juce::jlimit (0.0f, 48.0f, gap);
    layout.radius = juce::jlimit (0.0f, 80.0f, radius);
    layout.background = background | 0xff000000u;
}

bool Hub::choosePreset (int index)
{
    if (! juce::isPositiveAndBelow (index, numPresets))
        return false;

    const auto panels = presetAt (index).panels;

    // A track's slot never disappears from under it.
    if ((claimedMask.load() >> panels) != 0)
        return false;

    {
        const std::scoped_lock sl (layoutLock);
        layout.preset = index;
    }

    std::vector<std::function<void (int)>> setters;
    {
        const std::scoped_lock sl (memberLock);
        for (auto& m : members)
            if (m->controls.setCamerasParameter)
                setters.push_back (m->controls.setCamerasParameter);
    }

    for (auto& set : setters)
        set (panels);

    log ("grid template " + juce::String (presetAt (index).id));
    return true;
}

int Hub::effectivePreset (const GridLayout& look, int numSlots)
{
    numSlots = juce::jlimit (1, 9, numSlots);

    if (look.preset < 0)
        return presetForTemplate (look.legacyShape, numSlots);

    return presetAt (look.preset).panels == numSlots ? look.preset : defaultPresetFor (numSlots);
}

std::vector<Cell> Hub::cellsFor (const GridLayout& look, int numSlots)
{
    auto cells = presetLayout (effectivePreset (look, numSlots));
    const auto n = (int) cells.size();

    // The cell order is only honoured if it is a real permutation of the slots shown.
    uint32_t seen = 0;
    bool valid = true;
    for (int i = 0; i < n; ++i)
    {
        valid = valid && juce::isPositiveAndBelow (look.order[(size_t) i], n) && (seen & (1u << look.order[(size_t) i])) == 0;
        seen |= 1u << juce::jlimit (0, 8, look.order[(size_t) i]);
    }

    if (valid)
        for (auto& c : cells)
            c.slot = look.order[(size_t) c.slot];

    return cells;
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
    const auto active = activeSlots.load();
    const auto preset = effectivePreset (l, active);
    const auto size = canvasSize (l.canvas);
    const auto scale = (float) size.x / 1280.0f;

    auto o = std::make_unique<juce::DynamicObject>();
    o->setProperty ("canvas", (int) l.canvas);
    o->setProperty ("width", size.x);
    o->setProperty ("height", size.y);

    juce::Array<juce::var> sizes; // output size of every canvas, by canvas number
    for (int c = 0; c < 4; ++c)
    {
        const auto cs = canvasSize ((Canvas) c);
        sizes.add (juce::String (cs.x) + "\u00d7" + juce::String (cs.y));
    }
    o->setProperty ("sizes", sizes);
    o->setProperty ("preset", preset);
    o->setProperty ("presetId", juce::String (presetAt (preset).id));
    o->setProperty ("gap", (double) l.gap);
    o->setProperty ("radius", (double) l.radius);
    o->setProperty ("background", juce::String::toHexString ((int) (l.background & 0xffffffu)).paddedLeft ('0', 6));

    juce::Array<juce::var> order;
    for (auto v : l.order)
        order.add (v);
    o->setProperty ("order", order);

    // Where each cell sits in the picture, 0..1, frame included, so the UI can lay its overlays on top.
    juce::Array<juce::var> cells;
    for (const auto& c : cellsFor (l, active))
    {
        const auto r = framedCellArea (c.area, (float) size.x, (float) size.y, l.gap * scale);
        auto cell = std::make_unique<juce::DynamicObject>();
        cell->setProperty ("slot", c.slot);
        cell->setProperty ("x", r.getX() / (float) size.x);
        cell->setProperty ("y", r.getY() / (float) size.y);
        cell->setProperty ("w", r.getWidth() / (float) size.x);
        cell->setProperty ("h", r.getHeight() / (float) size.y);
        cells.add (juce::var (cell.release()));
    }
    o->setProperty ("cells", cells);
    return juce::var (o.release());
}

void Hub::applyLayoutVar (const juce::var& v)
{
    if (! v.isObject())
        return;

    const auto canvas = juce::jlimit (0, 3, (int) v.getProperty ("canvas", 0));
    const auto gap = (float) (double) v.getProperty ("gap", 4.0);
    const auto bg = (uint32_t) juce::String (v.getProperty ("background", "0b0b0f")).getHexValue32();
    setLayout ((Canvas) canvas, gap, bg, (float) (double) v.getProperty ("radius", 0.0));

    {
        // Layouts saved before the templates name a shape instead; it is matched to a template once the slot count is known.
        const std::scoped_lock sl (layoutLock);
        layout.preset = presetIndex (v.getProperty ("presetId", "").toString());
        layout.legacyShape = (Template) juce::jlimit (0, numTemplates - 1, (int) v.getProperty ("template", 0));
    }

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

    const auto scale = (float) size.x / 1280.0f;
    compositor.setFramed (true);
    compositor.setGapPixels (look.gap * scale);
    compositor.setCornerRadius (look.radius * scale);
    compositor.setBackground (look.background);

    const auto cells = cellsFor (look, active);

    compositor.render (output, cells, [&] (int slot) -> std::shared_ptr<const VideoFrame>
    {
        // A slot being recorded, or armed for a new take with none waiting in review, shows its live camera.
        // Others show their take, or the live camera if they have none.
        if (slotRecording[(size_t) slot].load()
            || (((armedMask.load() >> slot) & 1u) != 0 && ! e->slotHasReview (slot)))
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

std::shared_ptr<const ShownFrame> Hub::shownTake (int slot) const
{
    const std::scoped_lock sl (engineLock);
    if (engine == nullptr || ! juce::isPositiveAndBelow (slot, maxSlots))
        return nullptr;
    return engine->shown (slot);
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

    // Every take, newest first. The UI shows the newest few and the rest on request; past the cap the sync no longer
    // recognises a take, so the UI says so.
    std::set<int32_t> recognised;
    for (const auto& e : engine->history().playable (slot))
        recognised.insert (e.meta.takeId);

    for (const auto& e : engine->history().all (slot))
    {
        auto o = std::make_unique<juce::DynamicObject>();
        o->setProperty ("id", e.meta.takeId);
        o->setProperty ("recognised", recognised.count (e.meta.takeId) > 0);
        o->setProperty ("state", e.meta.state == TakeMeta::State::kept ? "kept"
                               : e.meta.state == TakeMeta::State::review ? "review" : "archived");
        o->setProperty ("seconds", e.meta.durationSeconds);
        o->setProperty ("slip", e.meta.slipSeconds);
        // Where the take sits in the project, for the takes timeline.
        o->setProperty ("startPpq", e.meta.startPpq);
        o->setProperty ("bpm", e.meta.bpm);
        o->setProperty ("timeSig", juce::String (e.meta.timeSigNumerator) + "/" + juce::String (e.meta.timeSigDenominator));
        o->setProperty ("fps", e.meta.fps);
        // The latency compensation applied to this take (before slip), for the chip under the canvas.
        o->setProperty ("latencyMs", (e.meta.audioLatencySeconds - e.meta.cameraLatencySeconds) * 1000.0);
        o->setProperty ("hasMov", e.folder.getChildFile ("take.mov").existsAsFile());
        o->setProperty ("heard", e.meta.trackName);
        list.add (juce::var (o.release()));
    }

    return list;
}

void Hub::setTrackName (Member* member, const juce::String& name)
{
    const std::scoped_lock sl (memberLock);
    for (auto& m : members)
        if (m.get() == member)
            m->trackName = name;
}

std::array<float, 9> Hub::slotLevels() const
{
    std::array<float, 9> levels {};
    const std::scoped_lock sl (memberLock);
    for (auto& m : members)
        if (const auto s = m->slot.load(); juce::isPositiveAndBelow (s, 9))
            levels[(size_t) s] = m->level.load (std::memory_order_relaxed);
    return levels;
}

juce::var Hub::takePeaks (int slot, int takeId) const
{
    juce::File folder;
    {
        const std::scoped_lock sl (engineLock);
        if (engine != nullptr && juce::isPositiveAndBelow (slot, maxSlots))
            for (const auto& e : engine->history().all (slot))
                if (e.meta.takeId == takeId)
                    folder = e.folder;
    }

    juce::Array<juce::var> list;
    if (folder != juce::File())
        for (auto p : griddy::takePeaks (folder, 64))
            list.add (p);
    return list;
}

juce::var Hub::getStatus (const Member* member) const
{
    const auto mySlot = member != nullptr ? member->slot.load() : -1;

    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty ("session", getSession());
    obj->setProperty ("sessionFolder", getSessionFolder().getFullPathName());
    obj->setProperty ("takesBytes", (juce::int64) takesBytes.load());
    obj->setProperty ("resetPending", mySlot >= 0 && resetRequested[(size_t) mySlot].load());
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
        const auto n = juce::String (mySlot + 1);
        if (audioEngine.load() == nullptr)
            hint = "Griddy's engine is not running yet.";
        else if (videoEngine.getSlotSources()[(size_t) mySlot].isEmpty())
            hint = "Pick a camera for slot " + n + " in the Slot tab.";
        else if (! member->playing.load())
            hint = "Armed. Press play in Live to record slot " + n + ".";
    }
    obj->setProperty ("recordHint", hint);
    obj->setProperty ("hostRecording", member != nullptr && member->hostRecording.load());
    obj->setProperty ("claimedMask", (int) claimedMask.load());
    obj->setProperty ("master", member != nullptr && member->isMaster.load());
    obj->setProperty ("layout", layoutToVar());

    {
        const std::scoped_lock sl (statusLock);
        obj->setProperty ("error", lastError);
        obj->setProperty ("pendingFolder", pendingFolder.getFullPathName());

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

    std::array<juce::String, maxSlots> names;
    {
        const std::scoped_lock sl (memberLock);
        for (auto& m : members)
            if (const auto s = m->slot.load(); juce::isPositiveAndBelow (s, maxSlots))
                names[(size_t) s] = m->trackName;
    }

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
                o->setProperty ("recordStartPpq", recordStartPpq[(size_t) s].load());
                o->setProperty ("trackName", names[(size_t) s]);
                o->setProperty ("armed", ((armedMask.load() >> s) & 1u) != 0);
                o->setProperty ("review", engine->slotHasReview (s));
                o->setProperty ("status", shown != nullptr ? statusName (shown->status) : "noTake");
                o->setProperty ("source", shown != nullptr ? kindName (shown->kind) : "none");
                o->setProperty ("takeId", shown != nullptr ? shown->takeId : -1);
                o->setProperty ("gap", shown != nullptr && shown->gap);
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
        {
            e->reloadSlot (slot);
            purgeLater();
        }
    });
}

#define GRIDDY_HUB_TAKE_ACTION(name, call)                      \
    bool Hub::name                                              \
    {                                                           \
        PlaybackEngine* e;                                      \
        { const std::scoped_lock sl (engineLock); e = engine.get(); } \
        if (e == nullptr || ! juce::isPositiveAndBelow (slot, maxSlots) || ! e->history().call) \
            return false;                                       \
        log ("slot " + juce::String (slot + 1) + ": " #name " take " + juce::String (id)); \
        reloadLater (slot);                                     \
        return true;                                            \
    }

GRIDDY_HUB_TAKE_ACTION (keepTake (int slot, int id), keep (slot, id))
GRIDDY_HUB_TAKE_ACTION (retakeTake (int slot, int id), retake (slot, id))
GRIDDY_HUB_TAKE_ACTION (restoreTake (int slot, int id), restore (slot, id))
GRIDDY_HUB_TAKE_ACTION (setSlip (int slot, int id, double seconds), setSlip (slot, id, seconds))

#undef GRIDDY_HUB_TAKE_ACTION

} // namespace griddy
