// Headless engine harness: no DAW, no GPU, no camera.
//
// It fakes the DAW transport, the record state, the audio each slot's track carries and the webcam,
// and it records real takes with the real TakeRecorder, then plays them back through the real
// PlaybackEngine. Pictures are coded: every frame carries its take label and frame number as black
// and white squares, so the harness can read what the engine is showing straight from the pixels
// and compare it with where the audio really is.
//
// Usage: GriddyHarness [scenario ...] [--soak-seconds N] [--big]   (no arguments runs everything but the soak)
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>

#include "TestSignals.h"
#include "engine/PlaybackEngine.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

#if JUCE_LINUX
 #include <sys/resource.h>
 #include <unistd.h>
#endif

using namespace griddy;
using griddy::testing::makeMusic;

namespace
{
constexpr double SR = 48000.0;
constexpr int BLK = 512;

int failures = 0;
juce::String currentScenario;
bool bigTakes = false;

void say (const juce::String& s) { std::cout << s << std::endl; }

void check (bool ok, const juce::String& what)
{
    say (juce::String (ok ? "  ok    " : "  FAIL  ") + what);
    if (! ok)
        ++failures;
}

void metric (const juce::String& name, double value, const juce::String& unit)
{
    say ("  METRIC " + currentScenario + " | " + name + " | " + juce::String (value, 3) + " " + unit);
}

void sleepUntil (double wall)
{
    for (;;)
    {
        const auto ms = (wall - steadyNowSeconds()) * 1000.0;
        if (ms <= 0.0)
            return;
        juce::Thread::sleep (ms > 2.0 ? (int) (ms - 1.0) : 0);
    }
}

//==============================================================================
// Coded pictures

juce::Image makeCodedFrame (int w, int h, int label, int counter)
{
    juce::Image img (juce::Image::ARGB, w, h, false, juce::SoftwareImageType());
    juce::Graphics g (img);
    g.fillAll (juce::Colour::fromHSV (std::fmod ((float) label * 0.137f, 1.0f), 0.5f, 0.45f, 1.0f));
    const auto cell = w / 24;

    for (int i = 0; i < 24; ++i)
    {
        const auto bit = i < 8 ? (label >> (7 - i)) & 1 : (counter >> (23 - i)) & 1;
        g.setColour (bit ? juce::Colours::white : juce::Colours::black);
        g.fillRect (i * cell, h / 20, cell, cell);
    }

    g.setColour (juce::Colours::white.withAlpha (0.5f));
    g.fillEllipse ((float) (counter % 60) * (float) w / 60.0f, (float) h * 0.6f, (float) h * 0.2f, (float) h * 0.2f); // something moving
    return img;
}

bool decodeCoded (const juce::Image& img, int& label, int& counter)
{
    if (! img.isValid())
        return false;

    const auto cell = img.getWidth() / 24;
    label = counter = 0;

    for (int i = 0; i < 24; ++i)
    {
        const auto x = i * cell + cell / 2, y = img.getHeight() / 20 + cell / 2;
        const auto bit = img.getPixelAt (x, y).getBrightness() > 0.5f ? 1 : 0;

        if (i < 8)
            label = (label << 1) | bit;
        else
            counter = (counter << 1) | bit;
    }

    return true;
}

struct SyntheticCamera final : juce::Thread
{
    SyntheticCamera (int w_, int h_, std::function<int()> lab, std::function<void (juce::Image, double)> out)
        : juce::Thread ("camera"), w (w_), h (h_), label (std::move (lab)), sink (std::move (out)) {}

    void run() override
    {
        auto next = steadyNowSeconds();
        int counter = 0;
        while (! threadShouldExit())
        {
            sink (makeCodedFrame (w, h, label(), counter++), steadyNowSeconds());
            next += 1.0 / 30.0;
            sleepUntil (next);
        }
    }

    int w, h;
    std::function<int()> label;
    std::function<void (juce::Image, double)> sink;
};

//==============================================================================
// Takes

struct Recorded
{
    TakeHistory::Entry entry;
    std::vector<float> audio; // what the DAW would have on its clip
    int label() const { return entry.meta.takeId & 0xff; }
    int32_t id() const { return entry.meta.takeId; }
};

Recorded recordTake (PlaybackEngine& eng, int slot, double seconds, const std::vector<float>& performance, double hostStartSeconds, bool embed = false)
{
    TakeRecorder rec (eng.history());
    TakeRecorder::Config cfg;
    cfg.embedSync = embed;
    if (! bigTakes)
    {
        cfg.width = 640; cfg.height = 360; cfg.proxyWidth = 320; cfg.proxyHeight = 180;
    }

    TakeMeta meta;
    meta.sampleRate = SR;
    meta.startHostSample = (int64_t) (hostStartSeconds * SR);
    meta.startPpq = hostStartSeconds * 2.0;
    meta.bpm = 120.0;
    rec.start (slot, meta, cfg);

    SyntheticCamera cam (cfg.width, cfg.height, [&] { return rec.currentTakeId() & 0xff; },
                         [&] (juce::Image img, double wall) { rec.pushFrame (std::move (img), wall); });
    cam.startThread();

    const auto blocks = (int) (seconds * SR / BLK);
    const auto t0 = steadyNowSeconds();

    for (int i = 0; i < blocks; ++i)
    {
        sleepUntil (t0 + (double) i * BLK / SR);
        rec.feedAudio (performance.data() + (size_t) i * BLK, BLK, meta.startHostSample + (int64_t) i * BLK);
    }

    cam.stopThread (2000);
    Recorded r;
    r.entry = *rec.stop();
    r.audio.assign (performance.begin(), performance.begin() + (long) blocks * BLK);

    if (embed)
    {
        double sr;
        readWav (r.entry.folder.getChildFile ("audio.wav"), r.audio, sr);
    }

    if (rec.framesDropped() > 0)
        say ("  note: recorder dropped " + juce::String ((int) rec.framesDropped()) + " frames");

    return r;
}

//==============================================================================
// Timeline and driver: the fake DAW

struct Clip
{
    const std::vector<float>* audio = nullptr;
    int32_t takeId = -1;
    double startHost = 0.0;
    double length = 0.0;     // seconds on the timeline
    double anchorHost = 0.0; // take position anchorTake at host time anchorHost
    double anchorTake = 0.0;
    double stretch = 1.0;    // take seconds per host second; 1.0 unless the clip is warped
    float gain = 1.0f;

    static Clip place (const Recorded& r, double startHost, double offset, double length)
    {
        Clip c;
        c.audio = &r.audio;
        c.takeId = r.id();
        c.startHost = c.anchorHost = startHost;
        c.anchorTake = offset;
        c.length = length;
        return c;
    }

    double takeAt (double host) const { return anchorTake + (host - anchorHost) * stretch; }
    bool covers (double host) const { return host >= startHost && host < startHost + length; }
};

struct Timeline
{
    std::vector<std::vector<Clip>> clips; // per slot

    explicit Timeline (int slots) : clips ((size_t) slots) {}

    struct Truth { bool exists = false; int32_t takeId = -1; double takeSeconds = 0.0; };

    Truth truth (int slot, double host) const
    {
        for (const auto& c : clips[(size_t) slot])
            if (c.covers (host))
                return { true, c.takeId, c.takeAt (host) };
        return {};
    }

    void render (int slot, double hostStartSample, int n, float* out) const
    {
        std::fill (out, out + n, 0.0f);

        for (const auto& c : clips[(size_t) slot])
        {
            for (int i = 0; i < n; ++i)
            {
                const auto host = (hostStartSample + i) / SR;
                if (! c.covers (host))
                    continue;

                const auto pos = c.takeAt (host) * SR;
                const auto idx = (int64_t) pos;
                if (idx < 0 || idx + 1 >= (int64_t) c.audio->size())
                    continue;

                const auto frac = (float) (pos - (double) idx);
                out[i] += c.gain * ((*c.audio)[(size_t) idx] * (1.0f - frac) + (*c.audio)[(size_t) idx + 1] * frac);
            }
        }
    }
};

struct Event
{
    enum Type { play, pause, seek, loopOn, loopOff, stretch, tempo } type;
    double t;       // seconds after the run started
    double a = 0.0, b = 0.0;
    int slot = 0, clip = 0;
};

class Driver final : public juce::Thread
{
public:
    Driver (PlaybackEngine& e, Timeline& tl, std::vector<Event> ev, double startHost)
        : juce::Thread ("fake DAW audio"), engine (e), timeline (tl), events (std::move (ev)), pos (startHost)
    {
        std::sort (events.begin(), events.end(), [] (const Event& x, const Event& y) { return x.t < y.t; });
    }

    double t0 = 0.0;
    double hostNow (double wall) const
    {
        const auto p = hostPos.load();
        if (! isPlaying.load())
            return p;
        return p + std::min (std::max (wall - blockWall.load(), 0.0), 0.25);
    }

    bool playing() const { return isPlaying.load(); }
    double bpm = 120.0;

    void run() override
    {
        t0 = steadyNowSeconds();
        auto due = t0;
        std::vector<float> buf (BLK);
        size_t nextEvent = 0;

        while (! threadShouldExit())
        {
            sleepUntil (due);
            const auto rel = steadyNowSeconds() - t0;

            while (nextEvent < events.size() && events[nextEvent].t <= rel)
                apply (events[nextEvent++], rel);

            int left = BLK;
            while (left > 0)
            {
                auto n = left;
                if (isPlaying.load() && loopOn)
                    n = std::min (n, std::max (1, (int) std::ceil ((loopEnd - pos) * SR)));

                const auto hostSample = (int64_t) std::llround (pos * SR);
                for (int s = 0; s < engine.numSlots(); ++s)
                {
                    if (isPlaying.load())
                        timeline.render (s, (double) hostSample, n, buf.data());
                    else
                        std::fill (buf.begin(), buf.end(), 0.0f);

                    engine.feedSlotAudio (s, buf.data(), n, hostSample, isPlaying.load());
                }

                hostPos = pos;
                blockWall = steadyNowSeconds();

                if (isPlaying.load())
                {
                    pos += (double) n / SR;
                    if (loopOn && pos >= loopEnd - 0.5 / SR)
                        pos = loopStart;
                }

                left -= n;
            }

            due += (double) BLK / SR;
        }
    }

private:
    void apply (const Event& e, double)
    {
        switch (e.type)
        {
            case Event::play:    isPlaying = true; break;
            case Event::pause:   isPlaying = false; break;
            case Event::seek:    pos = e.a; hostPos = pos; break;
            case Event::loopOn:  loopOn = true; loopStart = e.a; loopEnd = e.b; break;
            case Event::loopOff: loopOn = false; break;
            case Event::tempo:   bpm = e.a; break;
            case Event::stretch:
            {
                auto& c = timeline.clips[(size_t) e.slot][(size_t) e.clip];
                c.anchorTake = c.takeAt (pos);
                c.anchorHost = pos;
                c.stretch = e.a;
                break;
            }
        }
    }

    PlaybackEngine& engine;
    Timeline& timeline;
    std::vector<Event> events;
    double pos = 0.0;
    bool loopOn = false;
    double loopStart = 0.0, loopEnd = 0.0;
    std::atomic<bool> isPlaying { false };
    std::atomic<double> hostPos { 0.0 }, blockWall { 0.0 };
};

//==============================================================================
// Observation

struct Sample
{
    double t = 0.0;        // seconds since the run started
    double host = 0.0;
    bool truth = false;
    bool shown = false;
    bool labelOk = false;
    double errMs = 0.0;    // shown picture time minus where the audio is
    int counter = -1;
    int label = -1;
    SyncStatus status = SyncStatus::noTake;
    SyncKind kind = SyncKind::none;
    bool warp = false;
    bool playing = false;
};

struct World
{
    std::unique_ptr<PlaybackEngine> engine;
    juce::File root;
    std::map<int, TakeMeta> labelMeta;
    std::vector<std::unique_ptr<Recorded>> takes;

    Recorded& add (Recorded r)
    {
        labelMeta[r.label()] = r.entry.meta;
        takes.push_back (std::make_unique<Recorded> (std::move (r)));
        return *takes.back();
    }
};

std::unique_ptr<World> makeWorld (int slots, Canvas canvas = Canvas::hd720)
{
    auto w = std::make_unique<World>();
    w->root = juce::File::createTempFile ("griddy-harness");
    PlaybackEngine::Config c;
    c.numSlots = slots;
    c.sampleRate = SR;
    c.canvas = canvas;
    c.takesRoot = w->root;
    w->engine = std::make_unique<PlaybackEngine> (c);
    w->engine->start();
    return w;
}

void finish (World& w)
{
    w.engine->stop();
    w.root.deleteRecursively();
}

std::vector<Sample> observe (World& w, Timeline& tl, std::vector<Event> events, double startHost, double seconds, int slot)
{
    Driver driver (*w.engine, tl, std::move (events), startHost);
    driver.startThread (juce::Thread::Priority::high);
    juce::Thread::sleep (30);

    std::vector<Sample> out;
    auto next = steadyNowSeconds();

    while (steadyNowSeconds() - driver.t0 < seconds)
    {
        next += 0.033;
        sleepUntil (next);

        const auto wall = steadyNowSeconds();
        Sample s;
        s.t = wall - driver.t0;
        s.host = driver.hostNow (wall);
        s.playing = driver.playing();

        const auto truth = tl.truth (slot, s.host);
        s.truth = truth.exists;

        if (const auto f = w.engine->shown (slot); f != nullptr && f->frame != nullptr)
        {
            s.status = f->status;
            s.kind = f->kind;
            s.warp = f->warpSuspected;
            s.shown = decodeCoded (f->frame->image, s.label, s.counter);

            if (s.shown)
            {
                const auto it = w.labelMeta.find (s.label);
                if (it != w.labelMeta.end() && s.counter < (int) it->second.frameTimes.size())
                {
                    const auto shownTime = it->second.frameTimes[(size_t) s.counter] + it->second.videoOffsetSeconds();
                    s.labelOk = truth.exists && (truth.takeId & 0xff) == s.label;
                    if (s.labelOk)
                        s.errMs = (shownTime - truth.takeSeconds) * 1000.0;
                }
            }
        }

        out.push_back (s);
    }

    driver.stopThread (2000);
    return out;
}

//------------------------------------------------------------------------------
struct Stats { int n = 0; double p50 = 0, p99 = 0, max = 0, mean = 0; double badFraction = 0; };

Stats errStats (const std::vector<Sample>& s, double from, double to, double badMs = 100.0)
{
    std::vector<double> e;
    int total = 0, bad = 0;

    for (const auto& x : s)
        if (x.t >= from && x.t < to && x.truth && x.playing)
        {
            ++total;
            if (x.labelOk && std::abs (x.errMs) < badMs)
                e.push_back (std::abs (x.errMs));
            else
                ++bad;
        }

    Stats st;
    st.n = total;
    st.badFraction = total > 0 ? (double) bad / total : 0.0;
    if (! e.empty())
    {
        std::sort (e.begin(), e.end());
        st.p50 = e[e.size() / 2];
        st.p99 = e[(size_t) ((double) e.size() * 0.99)];
        st.max = e.back();
        for (auto v : e) st.mean += v;
        st.mean /= (double) e.size();
    }
    return st;
}

/** Seconds from `audioStart` (run time) until the picture is right and stays right for 3 samples. */
double lockTime (const std::vector<Sample>& s, double audioStart, double tolMs = 60.0)
{
    int run = 0;
    for (const auto& x : s)
    {
        if (x.t < audioStart)
            continue;

        if (x.truth && x.labelOk && std::abs (x.errMs) < tolMs)
        {
            if (++run >= 3)
                return x.t - audioStart - 2 * 0.033;
        }
        else
        {
            run = 0;
        }
    }
    return -1.0;
}

void report (const juce::String& name, const Stats& st)
{
    say ("  " + name + ": n " + juce::String (st.n) + ", |error| p50 " + juce::String (st.p50, 1) + " ms, p99 " + juce::String (st.p99, 1)
         + " ms, max " + juce::String (st.max, 1) + " ms, wrong " + juce::String (st.badFraction * 100.0, 1) + " percent");
    metric (name + " p99 error", st.p99, "ms");
    metric (name + " max error", st.max, "ms");
}

// A take's performance audio: 5 seconds unless said otherwise.
std::vector<float> performance (unsigned seed, double seconds = 5.0, double leadingSilence = 0.0)
{
    auto v = makeMusic (SR, seconds, seed);
    const auto n = (size_t) (leadingSilence * SR);
    for (size_t i = 0; i < n && i < v.size(); ++i)
        v[i] = 0.0f;
    return v;
}

void keepAndLoad (World& w, int slot, const Recorded& r)
{
    w.engine->history().keep (slot, r.id());
    w.engine->reloadSlot (slot);
}

//==============================================================================
// Scenarios

void playAndPause()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (1), 10.0));
    keepAndLoad (*w, 0, A);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (A, 10.0, 0.0, 5.0));
    const auto s = observe (*w, tl, { { Event::seek, 0.0, 9.5 }, { Event::play, 0.0 }, { Event::pause, 3.0 }, { Event::play, 4.5 } }, 9.5, 7.5, 0);

    report ("playing before pause", errStats (s, 0.6, 3.0));
    const auto lock = lockTime (s, 0.5);
    check (lock >= 0.0 && lock < 0.15, "picture right within 150 ms of the clip starting (took " + juce::String (lock, 3) + " s)");
    metric ("start lock time", lock * 1000.0, "ms");

    std::set<int> counters;
    int stoppedStatus = 0, n = 0;
    for (const auto& x : s)
        if (x.t > 3.2 && x.t < 4.4)
        {
            counters.insert (x.counter);
            ++n;
            stoppedStatus += x.status == SyncStatus::stopped ? 1 : 0;
        }
    check (counters.size() == 1, "paused: the picture holds still (" + juce::String ((int) counters.size()) + " distinct frames)");
    check (n > 0 && stoppedStatus == n, "paused: status says stopped");

    const auto after = errStats (s, 4.7, 7.0);
    report ("playing after pause", after);
    check (after.p99 < 45.0 && after.badFraction == 0.0, "resumes in sync after pause");
    finish (*w);
}

void loopWrap()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (2), 10.0));
    keepAndLoad (*w, 0, A);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (A, 10.0, 0.0, 5.0));
    const auto s = observe (*w, tl, { { Event::loopOn, 0.0, 11.0, 14.0 }, { Event::seek, 0.0, 10.2 }, { Event::play, 0.0 } }, 10.2, 11.0, 0);

    report ("through three loop wraps", errStats (s, 0.4, 11.0));
    for (double wrapAt : { 3.8, 6.8, 9.8 })
    {
        const auto st = errStats (s, wrapAt, wrapAt + 0.3);
        check (st.badFraction == 0.0 && st.max < 80.0, "after the wrap at " + juce::String (wrapAt, 1) + " s: max error " + juce::String (st.max, 1) + " ms");
    }
    finish (*w);
}

void seek()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (3), 10.0));
    keepAndLoad (*w, 0, A);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (A, 10.0, 0.0, 5.0));
    const auto s = observe (*w, tl,
        { { Event::seek, 0.0, 10.0 }, { Event::play, 0.0 }, { Event::seek, 1.0, 13.0 }, { Event::seek, 2.5, 10.5 },
          { Event::seek, 4.0, 17.0 }, { Event::seek, 5.5, 8.0 }, { Event::seek, 7.0, 12.0 } }, 10.0, 9.0, 0);

    for (double at : { 1.0, 2.5, 7.0 })
    {
        const auto st = errStats (s, at + 0.15, at + 1.2);
        check (st.badFraction == 0.0 && st.max < 80.0, "after the seek at " + juce::String (at, 1) + " s: max error " + juce::String (st.max, 1) + " ms");
    }

    int held = 0, n = 0;
    for (const auto& x : s)
        if ((x.t > 4.3 && x.t < 5.4) || (x.t > 5.8 && x.t < 6.9))
        {
            ++n;
            held += (x.status == SyncStatus::held || x.status == SyncStatus::stopped) ? 1 : 0;
        }
    check (n > 0 && held == n, "seeking outside the clip holds a frame and says so (" + juce::String (held) + "/" + juce::String (n) + ")");
    finish (*w);
}

void tempoChange()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (4), 10.0));
    keepAndLoad (*w, 0, A);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (A, 10.0, 0.0, 5.0));
    const auto s = observe (*w, tl, { { Event::seek, 0.0, 10.0 }, { Event::play, 0.0 }, { Event::tempo, 2.0, 90.0 } }, 10.0, 4.8, 0);
    const auto before = errStats (s, 0.3, 2.0), after = errStats (s, 2.1, 4.7);
    report ("before the tempo change", before);
    report ("after the tempo change", after);
    check (after.badFraction == 0.0 && after.p99 < 45.0, "an unwarped clip is unaffected by a tempo change (audio and video both keep real time)");
    finish (*w);
}

void recordSecondTake()
{
    auto w = makeWorld (1);
    auto& t1 = w->add (recordTake (*w->engine, 0, 5.0, performance (11), 10.0));
    keepAndLoad (*w, 0, t1);
    auto& t2 = w->add (recordTake (*w->engine, 0, 5.0, performance (12), 10.0));
    w->engine->reloadSlot (0); // take 2 is in review, so it plays at once

    check (w->engine->history().inReview (0).has_value() && w->engine->history().inReview (0)->meta.takeId == t2.id(), "the new take is in review");

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (t2, 10.0, 0.0, 5.0));          // the new audio replaced the old region
    tl.clips[0].push_back (Clip::place (t1, 30.0, 0.0, 5.0));          // an old copy of take 1 is still elsewhere
    const auto s = observe (*w, tl, { { Event::seek, 0.0, 9.8 }, { Event::play, 0.0 }, { Event::seek, 7.0, 29.8 } }, 9.8, 13.5, 0);

    const auto first = errStats (s, 0.5, 6.0);
    report ("new take over the old region", first);
    check (first.badFraction == 0.0, "the region plays take 2's video (take 1's video never appears there)");
    check (first.p99 < 45.0, "in sync");

    if (getenv ("GRIDDY_DEBUG"))
        for (const auto& x : s)
            if (x.t > 6.5 && x.t < 10.0 && ((int) (x.t * 30) % 6) == 0)
                say ("   dbg t " + juce::String (x.t, 2) + " host " + juce::String (x.host, 2) + " label " + juce::String (x.label) + " (want " + juce::String (t1.label()) + ") status " + juce::String ((int) x.status) + " kind " + juce::String ((int) x.kind) + " truth " + juce::String ((int) x.truth));
    const auto second = errStats (s, 7.9, 12.5);
    report ("old take still elsewhere on the timeline", second);
    check (second.badFraction < 0.1, "the old clip still plays take 1's video once its audio is recognised (wrong or late: " + juce::String (second.badFraction * 100, 1) + " percent)");
    const auto lock = lockTime (s, 7.2);
    metric ("archived take recognised after", lock * 1000.0, "ms");
    check (lock >= 0.0 && lock < 1.2, "archived take recognised within " + juce::String (lock, 2) + " s of its audio starting");

    w->engine->history().keep (0, t2.id());
    check (w->engine->history().active (0)->meta.takeId == t2.id(), "Keep makes take 2 the active take");
    check (t1.entry.folder.isDirectory() && t1.entry.folder.getChildFile ("frames/000000.jpg").existsAsFile(), "take 1 is archived on disk, never deleted");
    const auto listed = w->engine->history().listed (0);
    check (listed.size() == 2 && listed[1].meta.state == TakeMeta::State::archived, "history lists both takes, the old one archived");
    finish (*w);
}

void retakeDiscards()
{
    auto w = makeWorld (1);
    auto& t1 = w->add (recordTake (*w->engine, 0, 5.0, performance (21), 10.0));
    keepAndLoad (*w, 0, t1);
    auto& t2 = w->add (recordTake (*w->engine, 0, 5.0, performance (22), 10.0));

    check (w->engine->history().retake (0, t2.id()), "retake succeeds");
    check (! t2.entry.folder.exists(), "the discarded take's video is gone");
    check (w->engine->history().active (0).has_value() && w->engine->history().active (0)->meta.takeId == t1.id(), "the previous take is still the active take");
    check (w->engine->history().listed (0).size() == 1, "history lists one take");
    w->engine->reloadSlot (0);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (t1, 10.0, 0.0, 5.0));
    const auto s = observe (*w, tl, { { Event::seek, 0.0, 9.8 }, { Event::play, 0.0 } }, 9.8, 5.5, 0);
    const auto st = errStats (s, 0.5, 5.0);
    report ("after retake", st);
    check (st.badFraction == 0.0, "the kept take's video plays");
    finish (*w);
}

void duplicateEight()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (31), 10.0));
    keepAndLoad (*w, 0, A);

    Timeline tl (1);
    for (int i = 0; i < 8; ++i)
        tl.clips[0].push_back (Clip::place (A, 10.0 + 5.0 * i, 0.0, 5.0));

    const auto s = observe (*w, tl, { { Event::seek, 0.0, 9.8 }, { Event::play, 0.0 } }, 9.8, 41.5, 0);
    report ("one clip duplicated 8 times", errStats (s, 0.4, 40.5));

    for (int i = 1; i < 8; ++i)
    {
        const auto boundary = 0.2 + 5.0 * i;
        const auto st = errStats (s, boundary, boundary + 0.6);
        check (st.badFraction < 0.1 && st.max < 100.0, "repetition " + juce::String (i + 1) + " starts in sync (max " + juce::String (st.max, 1) + " ms, wrong " + juce::String (st.badFraction * 100, 0) + " percent)");
    }

    int jumps = w->engine->controller (0).numJumps();
    metric ("sync jumps over 8 repetitions", jumps, "");
    finish (*w);
}

void trimmedClip()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (41), 10.0));
    keepAndLoad (*w, 0, A);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (A, 20.0, 1.7, 2.5)); // take 1.7 to 4.2 s, placed far from where it was recorded
    const auto s = observe (*w, tl, { { Event::seek, 0.0, 19.5 }, { Event::play, 0.0 } }, 19.5, 5.0, 0);

    const auto lock = lockTime (s, 0.5);
    metric ("lock time after the trimmed clip starts", lock * 1000.0, "ms");
    check (lock >= 0.0 && lock < 0.7, "locks onto a trimmed, moved clip in " + juce::String (lock, 3) + " s");
    const auto st = errStats (s, 0.5 + std::max (lock, 0.0) + 0.1, 3.0);
    report ("trimmed clip after lock", st);
    check (st.badFraction == 0.0 && st.p99 < 45.0, "stays in sync");

    int held = 0, n = 0;
    for (const auto& x : s)
        if (x.t > 3.4)
        {
            ++n;
            held += x.status == SyncStatus::held ? 1 : 0;
        }
    check (n > 0 && held > n * 0.9, "after the trimmed clip ends the last frame is held (" + juce::String (held) + "/" + juce::String (n) + ")");
    finish (*w);
}

void splitAndMoved()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (51), 10.0));
    keepAndLoad (*w, 0, A);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (A, 10.0, 0.0, 2.5));   // first half stays
    tl.clips[0].push_back (Clip::place (A, 25.0, 2.5, 2.5));   // second half moved far away
    const auto s = observe (*w, tl,
        { { Event::seek, 0.0, 9.8 }, { Event::play, 0.0 }, { Event::seek, 3.2, 24.6 } }, 9.8, 7.0, 0);

    report ("first half, unmoved", errStats (s, 0.4, 2.9));
    const auto lock = lockTime (s, 3.2 + 0.4);
    metric ("lock time on the moved half", lock * 1000.0, "ms");
    check (lock >= 0.0 && lock < 0.7, "moved half locks in " + juce::String (lock, 3) + " s");
    const auto st = errStats (s, 3.6 + std::max (lock, 0.0) + 0.1, 6.0);
    report ("moved half after lock", st);
    check (st.badFraction == 0.0, "moved half stays in sync");
    finish (*w);
}

void silenceStart()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (61, 5.0, 1.5), 10.0));
    keepAndLoad (*w, 0, A);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (A, 10.0, 0.0, 5.0));          // where it was recorded
    tl.clips[0].push_back (Clip::place (A, 30.0, 0.0, 5.0));          // moved copy, silence first
    const auto s = observe (*w, tl,
        { { Event::seek, 0.0, 9.8 }, { Event::play, 0.0 }, { Event::seek, 6.0, 29.8 } }, 9.8, 12.0, 0);

    const auto silence = errStats (s, 0.4, 1.6);
    report ("recorded position, during the 1.5 s of silence", silence);
    check (silence.badFraction == 0.0 && silence.p99 < 45.0, "the host position prior shows the right frames before any sound arrives");
    const auto rest = errStats (s, 1.9, 5.0);
    report ("recorded position, after sound starts", rest);
    check (rest.badFraction == 0.0, "no wobble when the fingerprint takes over from the prior");

    const auto musicStart = 6.0 + 0.2 + 1.5;
    const auto lock = lockTime (s, musicStart);
    metric ("moved clip with silence first: lock after sound starts", lock * 1000.0, "ms");
    check (lock >= 0.0 && lock < 0.8, "a moved clip that starts with silence locks " + juce::String (lock, 3) + " s after its sound begins");
    finish (*w);
}

void identicalTakes()
{
    auto w = makeWorld (1);
    const auto perf = performance (71);
    auto& t1 = w->add (recordTake (*w->engine, 0, 5.0, perf, 10.0));
    keepAndLoad (*w, 0, t1);
    auto& t2 = w->add (recordTake (*w->engine, 0, 5.0, perf, 10.0));
    keepAndLoad (*w, 0, t2);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (t2, 10.0, 0.0, 5.0));
    const auto s = observe (*w, tl, { { Event::seek, 0.0, 9.8 }, { Event::play, 0.0 } }, 9.8, 5.5, 0);

    int changes = 0, last = -1;
    for (const auto& x : s)
        if (x.shown && x.t > 0.4)
        {
            if (last >= 0 && x.label != last)
                ++changes;
            last = x.label;
        }

    report ("two takes with identical audio", errStats (s, 0.4, 5.0));
    check (changes == 0, "the picture never flips between identical takes (" + juce::String (changes) + " flips)");
    check (last == t2.label(), "the newest take is the one shown");
    finish (*w);
}

void warpedClip()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (81, 5.0), 10.0));
    keepAndLoad (*w, 0, A);

    Timeline tl (1);
    auto c = Clip::place (A, 10.0, 0.0, 5.0);
    tl.clips[0].push_back (c);
    const auto s = observe (*w, tl,
        { { Event::seek, 0.0, 9.8 }, { Event::play, 0.0 }, { Event::stretch, 1.5, 1.12, 0.0, 0, 0 } }, 9.8, 5.2, 0);

    report ("before the clip is warped", errStats (s, 0.4, 1.6));
    double detected = -1.0;
    for (const auto& x : s)
        if (x.t > 1.7 && (x.status == SyncStatus::lost || x.warp))
        {
            detected = x.t - 1.7;
            break;
        }

    metric ("warp detected after", detected * 1000.0, "ms");
    check (detected >= 0.0 && detected < 4.0, "a warped clip is reported as lost sync, not shown as if nothing happened (" + juce::String (detected, 2) + " s)");
    finish (*w);
}

void embeddedSync()
{
    auto w = makeWorld (1);
    auto& E = w->add (recordTake (*w->engine, 0, 5.0, performance (91, 5.0), 10.0, true));
    keepAndLoad (*w, 0, E);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (E, 40.0, 1.0, 2.0));   // trimmed, moved
    tl.clips[0].push_back (Clip::place (E, 60.0, 0.0, 5.0));   // whole, moved
    tl.clips[0].push_back (Clip::place (E, 65.0, 0.0, 5.0));   // duplicated right after it
    const auto s = observe (*w, tl,
        { { Event::seek, 0.0, 39.5 }, { Event::play, 0.0 }, { Event::seek, 3.5, 59.5 } }, 39.5, 14.5, 0);

    const auto l1 = lockTime (s, 0.5);
    metric ("embedded lock, trimmed and moved", l1 * 1000.0, "ms");
    check (l1 >= 0.0 && l1 < 0.45, "trimmed and moved Griddy clip locks in " + juce::String (l1, 3) + " s");
    const auto l2 = lockTime (s, 4.0);
    metric ("embedded lock, whole clip moved", l2 * 1000.0, "ms");
    check (l2 >= 0.0 && l2 < 0.45, "moved Griddy clip locks in " + juce::String (l2, 3) + " s");

    int embeddedKinds = 0, n = 0;
    for (const auto& x : s)
        if (x.truth && x.t > 4.8 && x.t < 13.0)
        {
            ++n;
            embeddedKinds += x.kind == SyncKind::embedded ? 1 : 0;
        }
    check (n > 0 && embeddedKinds > n * 0.9, "the embedded signal is the source in use (" + juce::String (embeddedKinds) + "/" + juce::String (n) + ")");
    report ("duplicated Griddy clips", errStats (s, 4.8, 13.5));
    finish (*w);
}

//==============================================================================
// Soak and benchmarks

double processCpuSeconds()
{
#if JUCE_LINUX
    rusage u {};
    getrusage (RUSAGE_SELF, &u);
    return (double) u.ru_utime.tv_sec + (double) u.ru_utime.tv_usec * 1.0e-6 + (double) u.ru_stime.tv_sec + (double) u.ru_stime.tv_usec * 1.0e-6;
#else
    return 0.0;
#endif
}

double rssMegabytes()
{
#if JUCE_LINUX
    juce::File f ("/proc/self/statm");
    const auto parts = juce::StringArray::fromTokens (f.loadFileAsString(), " ", "");
    return parts.size() > 1 ? (double) parts[1].getLargeIntValue() * (double) sysconf (_SC_PAGESIZE) / 1048576.0 : 0.0;
#else
    return 0.0;
#endif
}

void soak (double seconds, int slots, Canvas canvas)
{
    auto w = makeWorld (slots, canvas);

    // Record all the takes at once from one camera: nine recorders sharing the same frames.
    std::vector<std::unique_ptr<TakeRecorder>> recs;
    std::vector<std::vector<float>> perfs;
    TakeRecorder::Config cfg;
    if (! bigTakes) { cfg.width = 640; cfg.height = 360; cfg.proxyWidth = 640 / 2; cfg.proxyHeight = 360 / 2; }
    else { cfg.proxyWidth = 640; cfg.proxyHeight = 360; }

    for (int i = 0; i < slots; ++i)
    {
        recs.push_back (std::make_unique<TakeRecorder> (w->engine->history()));
        perfs.push_back (performance (200 + (unsigned) i, 6.0));
        TakeMeta m;
        m.sampleRate = SR;
        m.startHostSample = (int64_t) (10.0 * SR);
        recs.back()->start (i, m, cfg);
    }

    SyntheticCamera cam (cfg.width, cfg.height, [] { return 0; }, [&] (juce::Image img, double wall)
    {
        for (auto& r : recs)
            r->pushFrame (img, wall);
    });
    cam.startThread();

    const auto t0 = steadyNowSeconds();
    for (int b = 0; b < (int) (6.0 * SR / BLK); ++b)
    {
        sleepUntil (t0 + (double) b * BLK / SR);
        for (int i = 0; i < slots; ++i)
            recs[(size_t) i]->feedAudio (perfs[(size_t) i].data() + (size_t) b * BLK, BLK, (int64_t) (10.0 * SR) + (int64_t) b * BLK);
    }

    cam.stopThread (2000);

    Timeline tl (slots);
    std::vector<std::unique_ptr<Recorded>> keep;
    for (int i = 0; i < slots; ++i)
    {
        auto r = std::make_unique<Recorded>();
        r->entry = *recs[(size_t) i]->stop();
        r->audio.assign (perfs[(size_t) i].begin(), perfs[(size_t) i].begin() + (long) (6.0 * SR / BLK) * BLK);
        w->engine->history().keep (i, r->id());
        w->engine->reloadSlot (i);
        tl.clips[(size_t) i].push_back (Clip::place (*r, 10.0, 0.0, 6.0));
        keep.push_back (std::move (r));
    }

    say ("  " + juce::String (slots) + " slots recorded; looping for " + juce::String (seconds, 0) + " s");

    Driver driver (*w->engine, tl, { { Event::loopOn, 0.0, 10.0, 16.0 }, { Event::seek, 0.0, 10.0 }, { Event::play, 0.0 } }, 10.0);
    driver.startThread (juce::Thread::Priority::high);

    const auto cpu0 = processCpuSeconds();
    const auto wall0 = steadyNowSeconds();
    std::vector<std::pair<double, double>> rss;
    uint64_t labelBad = 0, labelSamples = 0;
    double nextRss = 0.0;

    while (steadyNowSeconds() - wall0 < seconds)
    {
        juce::Thread::sleep (500);
        const auto t = steadyNowSeconds() - wall0;

        if (t >= nextRss)
        {
            rss.push_back ({ t, rssMegabytes() });
            nextRss += std::max (10.0, seconds / 60.0);
        }

        for (int i = 0; i < slots; ++i)
        {
            const auto f = w->engine->shown (i);
            int label, counter;
            if (f != nullptr && f->frame != nullptr && decodeCoded (f->frame->image, label, counter))
            {
                ++labelSamples;
                labelBad += label != (keep[(size_t) i]->label()) ? 1 : 0;
            }
        }
    }

    const auto wall = steadyNowSeconds() - wall0;
    const auto cpu = processCpuSeconds() - cpu0;
    driver.stopThread (2000);
    const auto rs = w->engine->renderStats();

    uint64_t skipped = 0, decodes = 0, sync = 0;
    double decodeMs = 0.0, decodeMax = 0.0, p99 = 0.0, maxDrift = 0.0;
    for (int i = 0; i < slots; ++i)
    {
        const auto d = w->engine->driftStats (i);
        const auto p = w->engine->playerStats (i);
        skipped += d.skippedFrames;
        decodes += p.decodes;
        sync += p.synchronousDecodes;
        decodeMs += p.decodeMsTotal;
        decodeMax = std::max (decodeMax, p.decodeMsMax);
        p99 = std::max (p99, d.p99Ms);
        maxDrift = std::max (maxDrift, d.maxMs);
    }

    metric ("run length", wall, "s");
    metric ("render frames", (double) rs.frames, "");
    metric ("late render frames", (double) rs.lateFrames, "");
    metric ("render mean", rs.renderMsMean, "ms");
    metric ("render max", rs.renderMsMax, "ms");
    metric ("skipped video frames (all slots)", (double) skipped, "");
    metric ("decodes", (double) decodes, "");
    metric ("on demand decodes", (double) sync, "");
    metric ("decode mean", decodes > 0 ? decodeMs / (double) decodes : 0.0, "ms");
    metric ("decode max", decodeMax, "ms");
    metric ("worst slot p99 drift", p99, "ms");
    metric ("max drift", maxDrift, "ms");
    metric ("CPU use (of one core)", wall > 0 ? 100.0 * cpu / wall : 0.0, "percent");
    metric ("wrong picture samples", (double) labelBad, "");

    double early = 0.0, late = 0.0;
    for (const auto& [t, mb] : rss)
        if (t >= std::min (60.0, seconds * 0.2) && early == 0.0)
            early = mb;
    if (! rss.empty())
        late = rss.back().second;
    for (const auto& [t, mb] : rss)
        say ("  rss at " + juce::String (t, 0) + " s: " + juce::String (mb, 1) + " MB");
    const auto growth = early > 0.0 ? 100.0 * (late - early) / early : 0.0;
    metric ("memory growth after warm up", growth, "percent");

    check (rs.frames > 0 && (double) rs.lateFrames / (double) rs.frames < 0.001, "no dropped render frames (late " + juce::String ((int) rs.lateFrames) + " of " + juce::String ((int) rs.frames) + ")");
    check (skipped == 0, "no skipped video frames (" + juce::String ((int) skipped) + ")");
    check (labelBad == 0, "every slot showed its own take for the whole run");
    check (std::abs (growth) < 1.0, "memory growth under 1 percent (" + juce::String (growth, 2) + ")");
    finish (*w);
}

void bench()
{
    say ("  microbenchmarks (single thread, this machine)");

    // Composite time at each canvas for 1, 4, 9 slots of proxy sized frames.
    for (auto canvas : { Canvas::hd720, Canvas::hd1080, Canvas::square, Canvas::portrait })
    {
        const auto size = canvasSize (canvas);
        for (int slots : { 1, 4, 9 })
        {
            juce::Image out (juce::Image::ARGB, size.x, size.y, true, juce::SoftwareImageType());
            std::vector<std::shared_ptr<const VideoFrame>> frames;
            for (int i = 0; i < slots; ++i)
            {
                auto f = std::make_shared<VideoFrame>();
                f->image = makeCodedFrame (640, 360, i, 0);
                frames.push_back (f);
            }

            Compositor comp;
            const auto cells = computeLayout (slots);
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            for (int i = 0; i < 30; ++i)
                comp.render (out, cells, [&] (int s) { return frames[(size_t) s]; });
            const auto ms = (juce::Time::getMillisecondCounterHiRes() - t0) / 30.0;
            metric ("CPU composite " + juce::String (size.x) + "x" + juce::String (size.y) + ", " + juce::String (slots) + " slots", ms, "ms per frame");
        }
    }

    // Decode cost at proxy and full size.
    for (auto sz : { std::pair<int, int> { 640, 360 }, { 1280, 720 }, { 1920, 1080 } })
    {
        juce::MemoryOutputStream mos;
        juce::JPEGImageFormat fmt;
        fmt.setQuality (0.85f);
        fmt.writeImageToStream (makeCodedFrame (sz.first, sz.second, 1, 1), mos);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < 60; ++i)
        {
            juce::MemoryInputStream in (mos.getMemoryBlock(), false);
            auto img = fmt.decodeImage (in).convertedToFormat (juce::Image::ARGB);
            if (! img.isValid()) break;
        }
        metric ("JPEG decode " + juce::String (sz.first) + "x" + juce::String (sz.second), (juce::Time::getMillisecondCounterHiRes() - t0) / 60.0, "ms per frame");
        metric ("JPEG size " + juce::String (sz.first) + "x" + juce::String (sz.second), (double) mos.getDataSize() / 1024.0, "KB");
    }

    // Sync analysis cost per second of audio, per slot.
    {
        auto music = makeMusic (SR, 60.0, 5);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        auto idx = std::make_shared<fingerprint::Index> (music.data(), music.size(), SR);
        metric ("fingerprint index build", (juce::Time::getMillisecondCounterHiRes() - t0) / 60.0, "ms per second of audio");
        metric ("fingerprint index memory", (double) (idx->entries.size() * 8 + idx->audio8k.size() * 2) / 1048576.0, "MB per minute of audio");

        fingerprint::FingerprintClipSource cs (SR);
        cs.setIndexes ({ idx });
        const auto t1 = juce::Time::getMillisecondCounterHiRes();
        for (size_t i = 0; i + BLK <= music.size() / 2; i += BLK)
        {
            cs.feed (music.data() + i, BLK, (int64_t) i, true);
            cs.process();
        }
        metric ("fingerprint live analysis and matching", (juce::Time::getMillisecondCounterHiRes() - t1) / 30.0, "ms per second of audio");

        embedded::EmbeddedSyncClipSource es (SR);
        const auto t2 = juce::Time::getMillisecondCounterHiRes();
        for (size_t i = 0; i + BLK <= music.size() / 2; i += BLK)
        {
            es.feed (music.data() + i, BLK, (int64_t) i, true);
            es.process();
        }
        metric ("embedded sync decoder", (juce::Time::getMillisecondCounterHiRes() - t2) / 30.0, "ms per second of audio");
    }
}

struct Scenario { const char* name; std::function<void()> run; bool inDefaultRun; };
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;

    double soakSeconds = 600.0;
    int soakSlots = 9;
    Canvas soakCanvas = Canvas::hd1080;
    std::vector<juce::String> wanted;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        if (a == "--soak-seconds" && i + 1 < argc) soakSeconds = juce::String (argv[++i]).getDoubleValue();
        else if (a == "--soak-slots" && i + 1 < argc) soakSlots = juce::String (argv[++i]).getIntValue();
        else if (a == "--soak-720") soakCanvas = Canvas::hd720;
        else if (a == "--big") bigTakes = true;
        else wanted.push_back (a);
    }

    const std::vector<Scenario> all {
        { "play_pause", playAndPause, true },
        { "loop_wrap", loopWrap, true },
        { "seek", seek, true },
        { "tempo_change", tempoChange, true },
        { "second_take", recordSecondTake, true },
        { "retake", retakeDiscards, true },
        { "duplicate_x8", duplicateEight, true },
        { "trimmed", trimmedClip, true },
        { "split_moved", splitAndMoved, true },
        { "silence_start", silenceStart, true },
        { "identical_takes", identicalTakes, true },
        { "warped", warpedClip, true },
        { "embedded", embeddedSync, true },
        { "bench", bench, true },
        { "soak", [&] { soak (soakSeconds, soakSlots, soakCanvas); }, false },
    };

    for (const auto& s : all)
    {
        const auto selected = wanted.empty() ? s.inDefaultRun : std::find (wanted.begin(), wanted.end(), juce::String (s.name)) != wanted.end();
        if (! selected)
            continue;

        currentScenario = s.name;
        say ("");
        say ("== " + currentScenario);
        s.run();
    }

    say ("");
    say (failures == 0 ? "HARNESS PASSED" : "HARNESS FAILED (" + juce::String (failures) + " checks)");
    return failures == 0 ? 0 : 1;
}
