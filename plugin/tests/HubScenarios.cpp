// Harness scenarios that run the whole Hub, the way a DAW with several Griddy instances does:
// one Hub, ten members (nine slots and a master), a fake transport driving every member's
// processBlock in real time, test pattern cameras, real take files in a temporary folder.
#include "HubScenarios.h"

#include "engine/Hub.h"

#include <cstdlib>
#include <set>

namespace griddy::harness
{
namespace
{
/** One fake plugin instance: its parameters, as the hub reads them. */
struct FakeInstance
{
    std::atomic<float> slot { 1.0f }, arm { 0.0f }, cameras { 4.0f }, master { 0.0f };
    Member* member = nullptr;

    MemberControls controls()
    {
        MemberControls c;
        c.slot = &slot;
        c.recordArm = &arm;
        c.cameras = &cameras;
        c.master = &master;
        c.setSlotParameter = [this] (int s) { slot = (float) (s + 1); };
        c.setMasterParameter = [this] (bool on) { master = on ? 1.0f : 0.0f; };
        c.setArmParameter = [this] (bool on) { arm = on ? 1.0f : 0.0f; };
        c.setCamerasParameter = [this] (int n) { cameras = (float) n; };
        return c;
    }
};

/** The DAW: every block, every instance hears the same transport. Silent tracks, like an empty track in Live. */
struct FakeDaw final : juce::Thread
{
    FakeDaw (Hub& h, std::vector<std::unique_ptr<FakeInstance>>& i, double rate)
        : juce::Thread ("fake DAW"), hub (h), instances (i), sr (rate) {}

    void run() override
    {
        juce::AudioBuffer<float> buffer (2, block);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        int64_t n = 0;

        while (! threadShouldExit())
        {
            buffer.clear();
            if (const auto a = tone.load(); a > 0.0f)
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    for (int i = 0; i < block; ++i)
                        buffer.setSample (ch, i, a * (float) std::sin ((double) (n * block + i) * 0.05));
            TransportSnapshot t;
            t.hostProvidedPosition = true;
            t.isPlaying = playing.load();
            t.isRecording = recording.load();
            t.timeInSamples = position.load();
            t.ppqPosition = (double) t.timeInSamples / sr * 2.0;
            t.updateCounter = ++n;

            for (auto& i : instances)
                if (i->member != nullptr)
                    hub.processBlock (*i->member, buffer, t);

            if (t.isPlaying)
                position += block;

            // Real time pacing.
            const auto due = t0 + (double) n * block / sr * 1000.0;
            const auto wait = due - juce::Time::getMillisecondCounterHiRes();
            if (wait > 1.0)
                juce::Thread::sleep ((int) wait);
        }
    }

    void playFrom (int64_t sample) { position = sample; playing = true; }
    void stop() { playing = false; }

    Hub& hub;
    std::vector<std::unique_ptr<FakeInstance>>& instances;
    double sr;
    static constexpr int block = 256;
    std::atomic<bool> playing { false }, recording { false };
    std::atomic<int64_t> position { 0 };
    std::atomic<float> tone { 0.0f }; // amplitude of a sine every track hears; 0 is silence
};

/** Points every Hub at a test folder for as long as it lives, so no scenario ever touches real takes in ~/Movies/Griddy. */
struct ScopedTakesFolder
{
    explicit ScopedTakesFolder (const juce::File& f)
        : previous (juce::SystemStats::getEnvironmentVariable ("GRIDDY_TAKES_FOLDER", {})),
          previousLive (juce::SystemStats::getEnvironmentVariable ("GRIDDY_LIVE_FOLDER", {}))
    {
        setenv ("GRIDDY_TAKES_FOLDER", f.getFullPathName().toRawUTF8(), 1);
        // And the Live script's files, so a Live running on this machine never changes a scenario.
        f.getChildFile ("Live").createDirectory();
        setenv ("GRIDDY_LIVE_FOLDER", f.getChildFile ("Live").getFullPathName().toRawUTF8(), 1);
    }

    ~ScopedTakesFolder()
    {
        if (previous.isEmpty())
            unsetenv ("GRIDDY_TAKES_FOLDER");
        else
            setenv ("GRIDDY_TAKES_FOLDER", previous.toRawUTF8(), 1);
        if (previousLive.isEmpty())
            unsetenv ("GRIDDY_LIVE_FOLDER");
        else
            setenv ("GRIDDY_LIVE_FOLDER", previousLive.toRawUTF8(), 1);
    }

    juce::String previous, previousLive;
};

void pump (double seconds)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;
    while (juce::Time::getMillisecondCounterHiRes() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
}

/** A hub with nine slot instances and a master, like the "Griddy 9 Track Master" template in Live. */
struct HubWorld
{
    explicit HubWorld (const Report& r, juce::File existingRoot = {}, juce::String session = {}) : report (r)
    {
        root = existingRoot != juce::File() ? existingRoot : juce::File::createTempFile ("griddy-hub");
        keepRoot = existingRoot != juce::File();
        root.createDirectory();
        folderGuard = std::make_unique<ScopedTakesFolder> (root);
        hub = std::make_unique<Hub>();

        for (int i = 0; i < 10; ++i)
        {
            auto inst = std::make_unique<FakeInstance>();
            inst->slot = (float) juce::jmin (i + 1, 9);
            inst->master = i == 9 ? 1.0f : 0.0f;
            instances.push_back (std::move (inst));
        }

        for (auto& i : instances)
            i->member = hub->addMember (i->controls());

        // Live prepares the instances first and loads their saved state afterwards.
        hub->prepare (44100.0, 256);
        if (session.isNotEmpty())
            hub->loadSession (nullptr, session, session::takeIdsOnDisk (root.getChildFile ("Sessions").getChildFile (session)));

        daw = std::make_unique<FakeDaw> (*hub, instances, 44100.0);
        daw->startThread (juce::Thread::Priority::high);
        pump (1.0);
    }

    ~HubWorld()
    {
        daw->stopThread (2000);
        for (auto& i : instances)
            if (i->member != nullptr)
                hub->removeMember (i->member);
        hub.reset();
        if (! keepRoot)
            root.deleteRecursively();
    }

    /** Watches what slot `slot` shows for `seconds` and returns the distinct frame indexes of its take. */
    std::set<int> watch (int slot, double seconds, int32_t& takeSeen)
    {
        std::set<int> frames;
        const auto end = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;
        while (juce::Time::getMillisecondCounterHiRes() < end)
        {
            juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
            if (const auto f = hub->shownTake (slot); f != nullptr && f->frame != nullptr)
            {
                frames.insert (f->frameIndex);
                takeSeen = f->takeId;
            }
        }
        return frames;
    }

    const Report& report;
    juce::File root;
    bool keepRoot = false;
    std::unique_ptr<ScopedTakesFolder> folderGuard;
    std::unique_ptr<Hub> hub;
    std::vector<std::unique_ptr<FakeInstance>> instances;
    std::unique_ptr<FakeDaw> daw;
};
} // namespace

// The test from Live on 12 November, from griddy.log: slot 1 armed, play from bar 1 for about 4.5 seconds on
// a silent track, stop. Ten seconds later play again without arm: the take must play. Then the one rule again:
// arm plus play records one new take and switches arm off, whatever the DAW's record flag says, and the
// master never records.
void hubRecordThenPlay (const Report& report)
{
    // First a set that records a take in slots 1 and 2, then a new set that loads the same session id (a template),
    // exactly as on that day: slot 1 held take 40 and slot 2 take 41, both waiting in review, when take 42 was recorded.
    const auto root = juce::File::createTempFile ("griddy-hub");
    juce::String session;
    {
        HubWorld first (report, root);
        session = first.hub->getSession();
        for (auto* inst : { first.instances[0].get(), first.instances[1].get() })
        {
            inst->arm = 1.0f;
            pump (0.3);
            first.daw->playFrom (0);
            pump (3.0);
            first.daw->stop();
            pump (1.5);
        }
    }

    HubWorld w (report, root, session);
    auto& one = *w.instances[0];
    report.check (w.hub->getSession() == session, "the second set opened the first set's session");

    // 22:55:19: play for five seconds without arm, so slot 1 plays its old take. The track has sound throughout, as it
    // does when Live records the clip with the take (the Live script links the two): with no sound, playback is black.
    w.daw->tone = 0.3f;
    w.daw->playFrom (0);
    pump (5.0);
    w.daw->stop();
    pump (2.0);

    one.arm = 1.0f;
    pump (0.5);
    w.daw->playFrom (0);
    pump (4.5);
    w.daw->stop();
    pump (1.0);

    report.check (! w.hub->isSlotRecording (0), "the take ended when the DAW stopped");
    report.check (one.arm.load() < 0.5f, "the one shot take switched arm off");

    int32_t take = -1;
    const auto held = w.watch (0, 2.0, take);
    report.check (take > 0 && held.size() == 1, "stopped: the slot holds one frame of its new take (" + juce::String ((int) held.size()) + " frames)");

    // The log does not show clicks in the take list. Keep and a slip nudge are what a user does next.
    if (std::getenv ("GRIDDY_HUB_KEEP") != nullptr)
    {
        report.check (w.hub->keepTake (0, take), "keep the new take");
        pump (0.5);
        w.hub->setSlip (0, take, 0.01);
    }

    pump (7.0);
    // A stuck picture shows one frame. A slow CI runner may show only a dozen in this time, so the bar is low.
    w.daw->playFrom (0);
    take = -1;
    const auto played = w.watch (0, 1.8, take);
    w.daw->stop();
    report.check (take > 0 && played.size() > 5, "play again: the take plays (" + juce::String ((int) played.size()) + " distinct frames in 1.8 s)");

    // Arm again and play, with the DAW's record flag off as Live always sends it: one new take, arm goes off.
    const auto before = take;
    pump (1.0);
    one.arm = 1.0f;
    pump (0.5);
    w.daw->playFrom (0);
    pump (2.0);
    report.check (w.hub->isSlotRecording (0), "arm plus play records, with no record flag from the DAW");
    w.daw->stop();
    pump (1.5);
    report.check (! w.hub->isSlotRecording (0) && one.arm.load() < 0.5f, "the second take also switched arm off");
    take = -1;
    w.watch (0, 0.5, take);
    report.check (take > before, "the slot now shows the new take (" + juce::String (take) + " after " + juce::String (before) + ")");

    // The record flag on, but not armed: nothing records.
    w.daw->recording = true;
    w.daw->playFrom (0);
    pump (1.5);
    report.check (! w.hub->isSlotRecording (0), "the DAW's record flag alone records nothing");
    w.daw->stop();
    w.daw->recording = false;
    pump (0.5);

    // The master has no slot, so arming it and playing records nothing anywhere.
    auto& master = *w.instances[9];
    master.arm = 1.0f;
    w.daw->playFrom (0);
    pump (1.5);
    bool any = false;
    for (int s = 0; s < 9; ++s)
        any = any || w.hub->isSlotRecording (s);
    report.check (! any, "an armed master records nothing");
    w.daw->stop();
    master.arm = 0.0f;
    pump (0.5);
    w.keepRoot = false;
}

// Sessions in one DAW process: a template opens empty, a second set takes over when the first one closes,
// the last instance leaving makes the next set fresh, and New session never deletes anything.
void hubSessions (const Report& report)
{
    const auto root = juce::File::createTempFile ("griddy-hub");
    juce::String used;
    {
        HubWorld set (report, root);
        used = set.hub->getSession();
        set.instances[0]->arm = 1.0f;
        pump (0.3);
        set.daw->playFrom (0);
        pump (2.0);
        set.daw->stop();
        pump (1.5);
    }

    const auto sessions = root.getChildFile ("Sessions");
    report.check (session::takeIdsOnDisk (sessions.getChildFile (used)).size() == 1, "the first set recorded one take");

    {
        // A new set made from a template that was saved with that session id and no takes.
        HubWorld set (report, root);
        for (auto& i : set.instances)
            set.hub->loadSession (i->member, used, std::vector<int32_t> {});
        pump (1.0);
        report.check (set.hub->getSession() != used, "a set made from the template gets a session of its own");
        report.check (set.hub->shownTake (0) == nullptr, "and shows no take from the other set");
        report.check (session::takeIdsOnDisk (sessions.getChildFile (used)).size() == 1, "the other set's take is still on disk");

        // The saved project itself: same id, same takes. All instances join one session.
        const auto ownTakes = session::takeIdsOnDisk (sessions.getChildFile (used));
        auto extra = std::make_unique<FakeInstance>();
        extra->slot = 9.0f;
        // A device pasted in from that project joins the open session instead of switching the grid.
        set.hub->loadSession (set.instances[0]->member, set.hub->getSession(), std::vector<int32_t> {});
        const auto before = set.hub->getSession();
        set.hub->loadSession (nullptr, used, ownTakes);
        report.check (set.hub->getSession() == before, "a device pasted in from another project joins the open session");

        // New session: an empty one; the old folder stays.
        report.check (set.hub->startNewSession(), "New session is accepted while nothing records");
        pump (0.5);
        report.check (set.hub->getSession() != before && sessions.getChildFile (before).isDirectory(), "New session starts a new folder and keeps the old one");
    }

    {
        // Every instance of the last set closed: the hub is shared per process, so the next set must not inherit its session.
        const ScopedTakesFolder guard (root);
        Hub hub;
        FakeInstance a;
        a.member = hub.addMember (a.controls());
        hub.prepare (44100.0, 256);
        hub.loadSession (a.member, used, session::takeIdsOnDisk (sessions.getChildFile (used)));
        report.check (hub.getSession() == used, "a saved project reopens its session");
        hub.removeMember (a.member);

        FakeInstance b;
        b.member = hub.addMember (b.controls());
        report.check (hub.getSession() != used, "after every instance closed, a new set starts a new session");

        // A host that loads the next set before closing the last one: when the last instance of the old project goes,
        // the new project's session opens.
        hub.loadSession (b.member, used, session::takeIdsOnDisk (sessions.getChildFile (used)));
        FakeInstance c;
        c.member = hub.addMember (c.controls());
        const auto other = juce::Uuid().toDashedString();
        hub.loadSession (c.member, other, std::vector<int32_t> {});
        report.check (hub.getSession() == used, "while the open project still has instances, a second project waits");
        hub.removeMember (b.member);
        report.check (hub.getSession() == other, "and opens once the first project's instances are gone");
        hub.removeMember (c.member);
    }

    root.deleteRecursively();
}

// Reset slot while a take records: the take ends and is kept on disk, arm goes off, the slot shows its live camera,
// and the take can be restored from the list.
void hubResetSlot (const Report& report)
{
    HubWorld w (report);
    auto& one = *w.instances[0];

    // A take to start from, then a second take that is reset while it records.
    one.arm = 1.0f;
    pump (0.3);
    w.daw->playFrom (0);
    pump (2.0);
    w.daw->stop();
    pump (1.5);
    int32_t first = -1;
    w.watch (0, 0.3, first);
    report.check (first > 0, "the slot shows its first take");
    report.check (w.hub->keepTake (0, first), "keep it, so the next take does not replace it");
    pump (0.5);

    one.arm = 1.0f;
    pump (0.3);
    w.daw->playFrom (0);
    pump (1.5);
    report.check (w.hub->isSlotRecording (0), "a second take is recording");
    w.hub->resetSlot (0);
    pump (2.0);

    report.check (! w.hub->isSlotRecording (0), "Reset slot ends the take in progress");
    report.check (one.arm.load() < 0.5f, "Reset slot switches arm off");
    pump (1.0);
    report.check (! w.hub->isSlotRecording (0), "nothing records again while the DAW keeps playing");
    w.daw->stop();
    pump (0.5);

    report.check (w.hub->shownTake (0) == nullptr, "the slot shows its live camera");
    const auto onDisk = session::takeIdsOnDisk (w.hub->getSessionFolder());
    report.check (onDisk.size() == 2, "both takes are still on disk (" + juce::String ((int) onDisk.size()) + ")");

    const auto listed = w.hub->getStatus (one.member).getProperty ("takes", {})[0];
    report.check (listed.size() == 2 && listed[0].getProperty ("state", {}).toString() == "archived"
                      && listed[1].getProperty ("state", {}).toString() == "archived",
                  "both takes are in the list, archived, ready to restore");

    report.check (w.hub->restoreTake (0, first), "restore the first take");
    int32_t shown = -1;
    w.watch (0, 1.0, shown);
    report.check (shown == first, "the restored take is back on screen");
}

void hubUiStatus (const Report& report)
{
    // What the Overdub Cam UI reads from the hub: track names, meters, where takes sit, the grid templates.
    HubWorld w (report);
    auto& one = *w.instances[0];

    w.hub->setTrackName (one.member, "Lead Vox");
    w.daw->tone = 0.5f;
    pump (0.5);
    const auto levels = w.hub->slotLevels();
    report.check (levels[0] > 0.3f && levels[0] <= 0.51f, "slot 1's meter shows what its track hears (" + juce::String (levels[0], 2) + ")");
    w.daw->tone = 0.0f;
    pump (1.0);
    report.check (w.hub->slotLevels()[0] < 0.05f, "the meter falls back when the track goes quiet");

    // A take from bar 3: the timeline needs its position.
    one.arm = 1.0f;
    pump (0.3);
    w.daw->playFrom (44100 * 4); // 4 seconds at 120 BPM is quarter note 8, bar 3
    pump (1.5);
    auto status = w.hub->getStatus (one.member);
    const auto slot0 = status.getProperty ("slots", {})[0];
    report.check (slot0.getProperty ("trackName", {}).toString() == "Lead Vox", "the status names slot 1's track");
    report.check (std::abs ((double) slot0.getProperty ("recordStartPpq", -1.0) - 8.0) < 0.5, "the take in progress says where it began");
    w.daw->stop();
    pump (2.0);

    status = w.hub->getStatus (one.member);
    const auto take = status.getProperty ("takes", {})[0][0];
    report.check (std::abs ((double) take.getProperty ("startPpq", -1.0) - 8.0) < 0.5, "the finished take says where it sits in the project");
    report.check (take.getProperty ("timeSig", {}).toString() == "4/4" && (double) take.getProperty ("bpm", 0.0) > 0.0, "the take carries tempo and time signature");
    report.check (w.hub->takePeaks (0, (int) take.getProperty ("id", -1)).size() == 64, "the take has 64 waveform peaks");

    // Nine tracks hold nine slots: a three panel template would hide six of them, so it is refused.
    report.check (! w.hub->choosePreset (presetIndex ("1p2")), "a template with fewer panels than the tracks' slots is refused");
    const auto cells = status.getProperty ("layout", {}).getProperty ("cells", {});
    report.check (cells.size() == 9, "the layout lists a rectangle for each of the nine cells");
    report.check ((double) cells[0].getProperty ("x", 0.0) > 0.0, "the frame runs round the edge, so the first cell starts inside it");

    // Track 1 asks for slot 2, which track 2 holds: they swap, and stay swapped.
    one.slot = 2.0f;
    pump (1.0);
    report.check (one.member->slot.load() == 1 && w.instances[1]->member->slot.load() == 0, "asking for a held slot swaps the two tracks");
    report.check (std::lround (w.instances[1]->slot.load()) == 1, "the other track's slot parameter shows its new slot");
    pump (1.0);
    report.check (one.member->slot.load() == 1 && w.instances[1]->member->slot.load() == 0, "the swap holds");
    one.slot = 1.0f;
    pump (1.0);
    report.check (one.member->slot.load() == 0 && w.instances[1]->member->slot.load() == 1, "and swaps back");

    // With only slots 1 and 2 held, the 1+2 template is allowed and every instance shows three cameras.
    w.daw->stopThread (2000);
    for (size_t i = 2; i < w.instances.size(); ++i)
    {
        w.hub->removeMember (w.instances[i]->member);
        w.instances[i]->member = nullptr;
    }
    w.daw->startThread (juce::Thread::Priority::high);
    pump (0.5);
    report.check (w.hub->choosePreset (presetIndex ("1p2")), "the 1+2 template is accepted");
    pump (0.5);
    report.check (std::lround (one.cameras.load()) == 3, "choosing it sets the instances to three cameras");
    const auto layout = w.hub->getStatus (one.member).getProperty ("layout", {});
    report.check (layout.getProperty ("presetId", {}).toString() == "1p2" && layout.getProperty ("cells", {}).size() == 3, "the grid is 1+2");
}

// The Live script's side of a recording, as on 18 November: a set saved before Griddy had its "Track armed in Live"
// parameter, so Live's arm reaches Griddy only through the arrangement file, and Live records before Griddy hears
// of it. Each take must start where Live's recording did.
void hubLiveRecordStart (const Report& report)
{
    HubWorld w (report);
    auto& one = *w.instances[0];
    const auto file = liveTimelineFile();

    // What the script writes: slot 1's track, armed or not, and where its recording began (beats) while Live records.
    auto writeLive = [&] (bool armed, bool recording, double recordFrom)
    {
        juce::String track = "{\"name\":\"1-Audio\",\"slot\":\"1\",\"armed\":" + juce::String (armed ? "true" : "false")
                           + ",\"audible\":true,\"arrangement\":true,\"clips\":[]"
                           + (recordFrom >= 0.0 ? ",\"recordFrom\":" + juce::String (recordFrom, 6) : juce::String()) + "}";
        file.replaceWithText ("{\"version\":1,\"written\":" + juce::String ((double) juce::Time::currentTimeMillis() / 1000.0, 3)
                              + ",\"recording\":" + (recording ? "true" : "false") + ",\"tracks\":[" + track + "]}");
    };
    auto takeStart = [&]
    {
        const auto takes = w.hub->getStatus (one.member).getProperty ("takes", {})[0];
        return takes.size() > 0 ? (double) takes[0].getProperty ("startPpq", -1.0) : -1.0;
    };
    auto takeCount = [&] { return w.hub->getStatus (one.member).getProperty ("takes", {})[0].size(); };
    const auto beatNow = [&] { return (double) w.daw->position.load() / 44100.0 * 2.0; };

    writeLive (false, false, -1.0);
    pump (0.5);
    report.check (w.hub->liveScriptRunning(), "the arrangement file says the Live script runs");
    report.check (! (bool) w.hub->getStatus (one.member).getProperty ("slots", {})[0].getProperty ("liveArmed", false), "not armed in Live");
    writeLive (true, false, -1.0);
    pump (0.3);
    report.check ((bool) w.hub->getStatus (one.member).getProperty ("slots", {})[0].getProperty ("liveArmed", false),
                  "armed in Live reaches Griddy through the file, in a set without the Track armed in Live parameter");

    // Punch in while Live plays: Live records from the press, Griddy's arm comes 0.15 s later.
    w.daw->tone = 0.3f;
    w.daw->playFrom (0);
    pump (1.0);
    const auto pressed = beatNow();
    writeLive (true, true, pressed);
    pump (0.15);
    one.arm = 1.0f;
    pump (1.5);
    report.check (w.hub->isSlotRecording (0), "Griddy records with Live");
    one.arm = 0.0f;
    writeLive (true, false, -1.0);
    pump (2.0);
    const auto start = takeStart();
    report.check (std::abs (start - pressed) < 0.03, "the take starts where Live's recording did, not when Griddy heard ("
                  + juce::String ((start - pressed) * 500.0, 1) + " ms off)");

    // And its picture starts there too: the frames Griddy kept while it had not heard yet are in the take.
    {
        juce::File newest;
        for (const auto& f : w.root.findChildFiles (juce::File::findFiles, true, "take.json"))
            if (newest == juce::File() || f.getLastModificationTime() > newest.getLastModificationTime())
                newest = f;
        const auto meta = juce::JSON::parse (newest.loadFileAsString());
        const auto* times = meta.getProperty ("frameTimes", {}).getArray();
        const auto first = times != nullptr && ! times->isEmpty() ? (double) times->getFirst() : 99.0;
        report.check (first < 0.1, "the take's first frame is from where Live's recording began ("
                      + juce::String ((int) std::lround (first * 1000.0)) + " ms in)");
    }

    // Griddy armed before Live's punch in point: the take waits for it.
    const auto before = takeCount();
    w.daw->stop();
    pump (0.3);
    w.daw->playFrom (44100 * 3); // beat 6
    writeLive (true, true, 8.0);
    one.arm = 1.0f;
    pump (0.6);
    report.check (! w.hub->isSlotRecording (0), "before Live's recording starts, Griddy waits");
    pump (1.6);
    report.check (w.hub->isSlotRecording (0), "and records from it");
    one.arm = 0.0f;
    writeLive (true, false, -1.0);
    w.daw->stop();
    pump (2.0);
    report.check (takeCount() == before + 1 && std::abs (takeStart() - 8.0) < 0.03,
                  "one take, starting at the punch in point (" + juce::String ((takeStart() - 8.0) * 500.0, 1) + " ms off)");

    // A script that does not say where the recording began: the take starts as it always did, when Griddy hears.
    w.daw->playFrom (0);
    pump (0.5);
    writeLive (true, true, -1.0);
    const auto heard = beatNow();
    one.arm = 1.0f;
    pump (1.5);
    one.arm = 0.0f;
    writeLive (true, false, -1.0);
    w.daw->stop();
    pump (2.0);
    report.check (std::abs (takeStart() - heard) < 0.2, "with no start in the file, the take starts when Griddy heard ("
                  + juce::String ((takeStart() - heard) * 500.0, 1) + " ms off)");
}

// Film: the Film button for a clip clicked in Live, as the Live script answers it. Griddy films only while Live plays the
// clip asked for, never touches the arm, ends at the clip's end, and writes the file and where in it the take starts.
void hubFilm (const Report& report)
{
    HubWorld w (report);
    auto& one = *w.instances[0];
    const auto file = liveTimelineFile();
    constexpr double sr = 44100.0;

    // The script's file: slot 1's track with a recording of /Live/Hats.wav from 4 to 8 s, from the file's start.
    auto writeLive = [&] (bool selected, bool loops, double filmFrom, double filmTo, bool recording)
    {
        juce::String clip = "{\"start\":4.0,\"end\":8.0,\"active\":true,\"file\":0,\"born\":1000.0"
                          + juce::String (loops ? "" : ",\"origin\":4.0") + (selected ? ",\"selected\":true,\"bar\":3" : "") + "}";
        juce::String track = "{\"name\":\"1-Audio\",\"slot\":\"1\",\"armed\":false,\"audible\":true,\"arrangement\":true,"
                             "\"files\":[\"/Live/Hats.wav\"],\"clips\":[" + clip + "]"
                           + (filmTo > 0.0 ? ",\"film\":{\"from\":" + juce::String (filmFrom, 6) + ",\"to\":" + juce::String (filmTo, 6) + "}" : juce::String())
                           + "}";
        file.replaceWithText ("{\"version\":1,\"written\":" + juce::String ((double) juce::Time::currentTimeMillis() / 1000.0, 3)
                              + ",\"recording\":" + (recording ? "true" : "false") + ",\"tracks\":[" + track + "]}");
    };
    auto film = [&] { return w.hub->getStatus (one.member).getProperty ("film", {}); };
    auto lastRequest = [&]
    {
        const auto v = juce::JSON::parse (liveRequestsFile().loadFileAsString());
        const auto* list = v.getProperty ("requests", {}).getArray();
        return list != nullptr && ! list->isEmpty() ? list->getLast() : juce::var();
    };
    auto newestMeta = [&]
    {
        juce::File newest;
        for (const auto& f : w.root.findChildFiles (juce::File::findFiles, true, "take.json"))
            if (newest == juce::File() || f.getLastModificationTime() > newest.getLastModificationTime())
                newest = f;
        return juce::JSON::parse (newest.loadFileAsString());
    };
    auto takeCount = [&] { return w.hub->getStatus (one.member).getProperty ("takes", {})[0].size(); };
    auto keepTimelineFresh = [&] (double seconds, std::function<void()> write)
    {
        for (double t = 0.0; t < seconds; t += 0.25)
        {
            write();
            pump (0.25);
        }
    };

    writeLive (false, false, 0.0, 0.0, false);
    pump (0.5);
    report.check ((bool) film().getProperty ("available", false), "with the Live script, Film is available on a track's Griddy");
    report.check (w.hub->requestFilm (one.member, -1).isNotEmpty(), "with no clip clicked in Live, Film says why and does nothing");
    if (getenv ("GRIDDY_DEBUG"))
        report.say ("   film status: " + juce::JSON::toString (film(), true));
    const auto listed = film().getProperty ("recordings", {});
    const auto* recs = listed.getArray();
    report.check (recs != nullptr && recs->size() == 1 && recs->getFirst().getProperty ("name", {}).toString() == "Hats"
                  && recs->getFirst().getProperty ("video", {}).toString() == "none",
                  "the Slot tab's list has the track's recording, with no video");

    writeLive (true, true, 0.0, 0.0, false);
    pump (0.3);
    report.check (w.hub->requestFilm (one.member, -1).contains ("loops"), "a clip that loops past its end cannot be filmed, and Film says so");

    writeLive (true, false, 0.0, 0.0, false);
    pump (0.3);
    const auto chosen = film().getProperty ("selected", {});
    report.check (chosen.getProperty ("name", {}).toString() == "Hats" && (int) chosen.getProperty ("bar", 0) == 3,
                  "the clip clicked in Live shows as what Film films for, with its bar");

    // Film: Griddy asks Live to play the clip. Until the script says Live plays it, nothing is filmed.
    const auto before = takeCount();
    w.daw->tone = 0.3f;
    w.daw->playFrom ((int64_t) (6.0 * sr));
    report.check (w.hub->requestFilm (one.member, -1).isEmpty(), "Film on the clicked clip is asked of Live");
    const auto asked = lastRequest();
    report.check (asked.getProperty ("action", {}).toString() == "film" && (double) asked.getProperty ("from", 0.0) == 4.0
                  && (double) asked.getProperty ("to", 0.0) == 8.0, "the request names the clip's start and end");
    pump (0.4);
    report.check (! w.hub->isSlotRecording (0), "while Live has not said it plays the clip, Griddy does not film where Live was");

    // The script jumps a bar before the clip (2 s at 120 BPM) and says so.
    w.daw->playFrom ((int64_t) (2.0 * sr));
    keepTimelineFresh (1.0, [&] { writeLive (true, false, 2.0, 8.0, false); });
    report.check (w.hub->isSlotRecording (0), "once Live plays the clip, Griddy films");
    report.check ((bool) film().getProperty ("filming", false), "and the status says it films, for the tile's \"FILM\"");
    report.check (one.arm.load() < 0.5f, "without arming anything");
    keepTimelineFresh (5.5, [&] { writeLive (true, false, 2.0, 8.0, false); });
    report.check (! w.hub->isSlotRecording (0), "at the clip's end the film take ends, even before Live stops");
    w.daw->stop();
    writeLive (true, false, 0.0, 0.0, false);
    pump (2.0);
    report.check (takeCount() == before + 1, "one film take");
    {
        const auto meta = newestMeta();
        const auto f = meta.getProperty ("filmedFor", {});
        const auto start = (double) (juce::int64) meta.getProperty ("startHostSample", 0) / (double) meta.getProperty ("sampleRate", sr);
        const auto lead = (double) f.getProperty ("lead", 99.0);
        report.check (f.getProperty ("path", {}).toString() == "/Live/Hats.wav" && (double) f.getProperty ("born", 0.0) == 1000.0,
                      "the take says which file it was filmed for");
        report.check (std::abs (lead - (start - 4.0)) < 1.0e-6 && std::abs (lead + 2.0) < 0.03,
                      "and where in it the take starts: " + juce::String (lead, 3) + " s, from where filming began");
    }
    report.check (! (bool) film().getProperty ("armed", true), "one take per press: Film is ready again");
    report.check (film().getProperty ("recordings", {})[0].getProperty ("video", {}).toString() == "filmed",
                  "and the recording now shows as filmed");

    // Stop film early.
    w.daw->playFrom ((int64_t) (2.0 * sr));
    w.hub->requestFilm (one.member, -1);
    keepTimelineFresh (1.0, [&] { writeLive (true, false, 2.0, 8.0, false); });
    report.check (w.hub->isSlotRecording (0) && w.hub->stopFilm (one.member), "Stop film while filming");
    pump (0.3);
    report.check (! w.hub->isSlotRecording (0) && lastRequest().getProperty ("action", {}).toString() == "filmstop",
                  "ends the take at once and asks Live to stop");
    w.daw->stop();
    writeLive (true, false, 0.0, 0.0, false);
    pump (1.5);

    // Live starts recording during filming: filming ends.
    w.daw->playFrom ((int64_t) (2.0 * sr));
    w.hub->requestFilm (one.member, -1);
    keepTimelineFresh (0.75, [&] { writeLive (true, false, 2.0, 8.0, false); });
    keepTimelineFresh (0.5, [&] { writeLive (true, false, 2.0, 8.0, true); });
    report.check (! w.hub->isSlotRecording (0) && ! (bool) film().getProperty ("armed", true), "Live recording during filming ends the film");
    w.daw->stop();
    writeLive (true, false, 0.0, 0.0, false);
    pump (1.5);

    // Live never plays it.
    const auto count = takeCount();
    w.hub->requestFilm (one.member, -1);
    keepTimelineFresh (3.5, [&] { writeLive (true, false, 0.0, 0.0, false); });
    report.check (! (bool) film().getProperty ("armed", true) && takeCount() == count, "Film gives up when Live never plays the clip");
}

} // namespace griddy::harness

