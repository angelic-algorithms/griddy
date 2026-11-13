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
    std::atomic<float> slot { 1.0f }, arm { 0.0f }, mode { 1.0f }, cameras { 4.0f }, master { 0.0f };
    Member* member = nullptr;

    MemberControls controls()
    {
        MemberControls c;
        c.slot = &slot;
        c.recordArm = &arm;
        c.recordMode = &mode;
        c.cameras = &cameras;
        c.master = &master;
        c.setSlotParameter = [this] (int s) { slot = (float) (s + 1); };
        c.setMasterParameter = [this] (bool on) { master = on ? 1.0f : 0.0f; };
        c.setArmParameter = [this] (bool on) { arm = on ? 1.0f : 0.0f; };
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
};

/** Points every Hub at a test folder for as long as it lives, so no scenario ever touches real takes in ~/Movies/Griddy. */
struct ScopedTakesFolder
{
    explicit ScopedTakesFolder (const juce::File& f) : previous (juce::SystemStats::getEnvironmentVariable ("GRIDDY_TAKES_FOLDER", {}))
    {
        setenv ("GRIDDY_TAKES_FOLDER", f.getFullPathName().toRawUTF8(), 1);
    }

    ~ScopedTakesFolder()
    {
        if (previous.isEmpty())
            unsetenv ("GRIDDY_TAKES_FOLDER");
        else
            setenv ("GRIDDY_TAKES_FOLDER", previous.toRawUTF8(), 1);
    }

    juce::String previous;
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

// The test from Live on 12 November, from griddy.log: Record when DAW plays, slot 1 armed, play from bar 1
// for about 4.5 seconds on a silent track, stop. Ten seconds later play again without arm: the take must play.
// Then arm again, switch Record when to DAW records and play without the DAW recording: no take starts,
// and the take still plays.
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

    // 22:55:19: play for five seconds without arm, so slot 1 plays its old take.
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
    w.daw->playFrom (0);
    take = -1;
    const auto played = w.watch (0, 1.8, take);
    w.daw->stop();
    report.check (take > 0 && played.size() > 20, "play again: the take plays (" + juce::String ((int) played.size()) + " distinct frames in 1.8 s)");

    pump (1.0);
    one.arm = 1.0f;
    pump (0.5);
    one.mode = 0.0f;
    pump (0.5);
    w.daw->playFrom (0);
    take = -1;
    const auto again = w.watch (0, 2.5, take);
    w.daw->stop();
    report.check (! w.hub->isSlotRecording (0), "DAW records without the DAW recording starts no take");
    report.check (take > 0 && again.size() > 25, "armed in DAW records mode, the take still plays (" + juce::String ((int) again.size()) + " distinct frames in 2.5 s)");
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

} // namespace griddy::harness
