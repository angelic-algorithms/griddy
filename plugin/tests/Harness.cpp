// Headless engine harness: no DAW, no GPU, no camera.
//
// It fakes the DAW transport, the record state, the audio each slot's track carries and the webcam,
// and it records real takes with the real TakeRecorder, then plays them back through the real
// PlaybackEngine. Pictures are coded: every frame carries its take label and frame number as black
// and white squares, so the harness can read what the engine is showing straight from the pixels
// and compare it with where the audio really is.
//
// Usage: GriddyHarness [scenario ...] [--soak-seconds N] [--big]   (no arguments runs everything but the soak)
#include <cstdlib>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>

#include "HubScenarios.h"
#include "TestSignals.h"
#include "capture/CaptureSource.h"
#include "engine/GpuCompositor.h"
#include "engine/GridExport.h"
#include "engine/MovTake.h"
#include "engine/PlaybackEngine.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <thread>

#if JUCE_LINUX || JUCE_MAC
 #include <sys/resource.h>
 #include <unistd.h>
#endif
#if JUCE_MAC
 #include <mach/mach.h>
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
    bool movOk = false;
    juce::File flat; // the plainly named copy of take.mov, when one was asked for
    double stopSeconds = 0.0; // how long TakeRecorder::stop() took
    uint64_t dropped = 0;     // camera frames the recorder dropped
    int label() const { return entry.meta.takeId & 0xff; }
    int32_t id() const { return entry.meta.takeId; }
};

Recorded recordTake (PlaybackEngine& eng, int slot, double seconds, const std::vector<float>& performance, double hostStartSeconds, bool embed = false, bool wantMov = false, double loopSeconds = 0.0)
{
    TakeRecorder rec (eng.history());
    TakeRecorder::Config cfg;
    cfg.embedSync = embed;
    cfg.exportMov = wantMov;
    if (wantMov)
        cfg.flatFolder = eng.history().slotFolder (slot).getParentDirectory().getChildFile ("Takes");
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
        // A looping DAW sends the host position back to the loop start at every wrap.
        auto offset = (int64_t) i * BLK;
        if (loopSeconds > 0.0)
            offset %= (int64_t) (loopSeconds * SR);
        rec.feedAudio (performance.data() + (size_t) i * BLK, BLK, meta.startHostSample + offset);
    }

    cam.stopThread (2000);
    Recorded r;
    const auto stopStart = steadyNowSeconds();
    r.entry = *rec.stop();
    r.stopSeconds = steadyNowSeconds() - stopStart;
    r.movOk = wantMov && rec.waitForMovExport();
    r.flat = rec.flatCopy();
    if (wantMov && ! r.movOk)
        say ("  take.mov export failed: " + rec.movExportError());
    r.audio.assign (performance.begin(), performance.begin() + (long) blocks * BLK);

    if (embed)
    {
        double sr;
        readWav (r.entry.folder.getChildFile ("audio.wav"), r.audio, sr);
    }

    r.dropped = rec.framesDropped();
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
    double fadeIn = 0.0, fadeOut = 0.0; // seconds of linear fade at the clip's edges, as a crossfade in the DAW makes

    float fadeAt (double host) const
    {
        auto g = 1.0;
        if (fadeIn > 0.0)
            g = std::min (g, (host - startHost) / fadeIn);
        if (fadeOut > 0.0)
            g = std::min (g, (startHost + length - host) / fadeOut);
        return (float) std::clamp (g, 0.0, 1.0);
    }

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
                out[i] += c.gain * c.fadeAt (host) * ((*c.audio)[(size_t) idx] * (1.0f - frac) + (*c.audio)[(size_t) idx + 1] * frac);
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
    bool gap = false;      // the engine says no clip plays: black
    bool black = false;    // the picture really is black
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
            s.gap = f->gap;
            s.black = f->frame->image.isValid() && f->frame->image.getPixelAt (f->frame->image.getWidth() / 2, f->frame->image.getHeight() / 2).getBrightness() < 0.02f
                      && f->frame->image.getPixelAt (1, 1).getBrightness() < 0.02f;
            s.shown = ! f->gap && decodeCoded (f->frame->image, s.label, s.counter); // black is no take

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

        if (getenv ("GRIDDY_DEBUG"))
        {
            TakeChange c;
            while (w.engine->popTakeChange (slot, c))
                say ("   engine t " + juce::String (s.t, 3) + " host " + juce::String (c.hostSeconds, 3) + ": " + juce::String (c.oldTake) + "/" + juce::String (c.oldPass)
                     + " -> " + juce::String (c.newTake) + "/" + juce::String (c.newPass) + " (" + TakeChange::pathName (c.path) + ")");
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

// Griddy records on arm plus play, so the DAW may have recorded no audio: the track is silent on playback and only the
// host position can place the video. It must still follow play, pause, loop and seek.
void silentPlayback()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (41), 10.0));
    w->engine->reloadSlot (0); // still in review, exactly as after a stop in the plugin

    Timeline tl (1);
    auto c = Clip::place (A, 10.0, 0.0, 5.0);
    c.gain = 0.0f; // no audio on the track
    tl.clips[0].push_back (c);

    const auto s = observe (*w, tl,
        { { Event::seek, 0.0, 10.0 }, { Event::play, 0.0 }, { Event::pause, 1.5 }, { Event::play, 2.5 },
          { Event::loopOn, 4.0, 11.0, 13.0 }, { Event::seek, 7.5, 10.5 } }, 10.0, 9.5, 0);

    const auto first = errStats (s, 0.3, 1.5);
    report ("silent track, first play", first);
    check (first.n > 0 && first.badFraction == 0.0 && first.p99 < 60.0, "silent track: the take plays from the host position");

    std::set<int> counters;
    for (const auto& x : s)
        if (x.t > 1.7 && x.t < 2.4)
            counters.insert (x.counter);
    check (counters.size() == 1, "silent track: pause holds one frame (" + juce::String ((int) counters.size()) + " distinct)");

    const auto resumed = errStats (s, 2.7, 4.0);
    report ("silent track, after play again", resumed);
    check (resumed.badFraction == 0.0 && resumed.p99 < 60.0, "silent track: play again resumes in sync");

    const auto looped = errStats (s, 4.2, 7.4);
    report ("silent track, through loop wraps", looped);
    check (looped.badFraction < 0.05 && looped.p99 < 80.0, "silent track: follows the loop");

    const auto sought = errStats (s, 7.7, 9.4);
    report ("silent track, after a seek", sought);
    check (sought.badFraction < 0.05 && sought.p99 < 80.0, "silent track: follows a seek");
    finish (*w);
}

// The DAW loops a 2 second region while a 7 second take records (what Live does with the loop switch on).
// Frame times must follow the take audio, which runs straight on, not the host position, which jumps back.
void loopRecord()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 7.0, performance (51, 7.0), 10.0, false, false, 2.0));
    const auto& ft = A.entry.meta.frameTimes;

    bool rising = ft.size() > 10;
    for (size_t i = 1; i < ft.size(); ++i)
        rising = rising && ft[i] > ft[i - 1];
    check (rising, "frame times only go forward through the loop wraps");
    check (! ft.empty() && ft.front() > -0.2 && ft.front() < 0.2, "first frame near the start of the take (" + juce::String (ft.empty() ? 0.0 : ft.front(), 3) + " s)");
    check (! ft.empty() && std::abs (ft.back() - A.entry.meta.durationSeconds) < 0.3,
           "last frame near the end of the take (" + juce::String (ft.empty() ? 0.0 : ft.back(), 2) + " of " + juce::String (A.entry.meta.durationSeconds, 2) + " s)");

    // Played back from where it was recorded, without the loop, the picture moves with the audio.
    w->engine->reloadSlot (0);
    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (A, 10.0, 0.0, 7.0));
    const auto s = observe (*w, tl, { { Event::seek, 0.0, 10.0 }, { Event::play, 0.0 } }, 10.0, 6.5, 0);
    const auto st = errStats (s, 0.4, 6.3);
    report ("take recorded through loop wraps", st);
    check (st.badFraction == 0.0 && st.p99 < 60.0, "a take recorded while looping plays in sync");
    finish (*w);
}

// A new take replaces a take still in review. The old one may be huge (a long take is tens of thousands
// of frame files); archiving it must not delay the new take becoming playable. It used to be discarded; Live keeps the
// replaced recording on its take lanes, so now it is archived and stays recognisable.
void replaceBigReview()
{
    auto w = makeWorld (1);
    auto& old = w->add (recordTake (*w->engine, 0, 2.0, performance (61, 2.0), 10.0));

    // Bulk it up like a half hour take: 30000 more frame files.
    const juce::MemoryBlock junk (2048, true);
    for (int i = 0; i < 30000; ++i)
        old.entry.folder.getChildFile ("frames").getChildFile (juce::String::formatted ("x%06d.jpg", i)).replaceWithData (junk.getData(), junk.getSize());

    auto& next = w->add (recordTake (*w->engine, 0, 3.0, performance (62, 3.0), 10.0));
    metric ("stop with a big take to discard", next.stopSeconds * 1000.0, "ms");
    check (next.stopSeconds < 0.5, "a new take finishes at once even when a big review take is replaced (" + juce::String (next.stopSeconds, 3) + " s)");
    {
        TakeMeta m;
        check (readTakeMeta (old.entry.folder, m) && m.state == TakeMeta::State::archived, "the replaced take is archived at once, not deleted");
        check (w->engine->history().inReview (0).has_value() && w->engine->history().inReview (0)->meta.takeId == next.id(), "the new take is the one in review");
    }

    const auto t0 = steadyNowSeconds();
    w->engine->reloadSlot (0);
    const auto reload = steadyNowSeconds() - t0;
    metric ("reload after a take", reload * 1000.0, "ms");
    check (reload < 0.5, "the new take loads for playback in " + juce::String (reload, 3) + " s");

    TakeHistory::purgeDiscarded (w->engine->history().root());
    check (w->engine->history().slotFolder (0).findChildFiles (juce::File::findDirectories, false, "*.discarded").isEmpty(), "purging deletes the discarded media");
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
    check (l1 >= 0.0 && l1 < 0.15, "trimmed and moved Griddy clip locks in " + juce::String (l1, 3) + " s");
    const auto l2 = lockTime (s, 4.0);
    metric ("embedded lock, whole clip moved", l2 * 1000.0, "ms");
    check (l2 >= 0.0 && l2 < 0.15, "moved Griddy clip locks in " + juce::String (l2, 3) + " s");

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


double processCpuSeconds();
double rssMegabytes();

//==============================================================================
// Comping: several takes recorded over the same stretch, pieces of each on the timeline

// A take written straight to disk, without recording in real time: many takes for load tests. Small coded frames at 10 fps.
Recorded fakeTake (PlaybackEngine& eng, int slot, const std::vector<float>& perf, double hostStart)
{
    constexpr int w = 192, h = 108;
    TakeMeta meta;
    meta.sampleRate = SR;
    meta.startHostSample = (int64_t) (hostStart * SR);
    meta.startPpq = hostStart * 2.0;
    meta.bpm = 120.0;
    meta.width = meta.proxyWidth = w;
    meta.height = meta.proxyHeight = h;
    meta.fps = 10.0;
    auto e = eng.history().beginTake (slot, meta);

    {
        WavWriter wav;
        wav.open (e.folder.getChildFile ("audio.wav"), SR);
        wav.write (perf.data(), (int) perf.size());
        wav.close();
    }

    const auto seconds = (double) perf.size() / SR;
    const auto n = (int) (seconds * meta.fps);
    juce::JPEGImageFormat jpeg;
    for (int i = 0; i < n; ++i)
    {
        const auto img = makeCodedFrame (w, h, e.meta.takeId & 0xff, i);
        for (auto sub : { "frames", "proxy" })
        {
            juce::FileOutputStream out (e.folder.getChildFile (sub).getChildFile (juce::String::formatted ("%06d.jpg", i)));
            jpeg.writeImageToStream (img, out);
        }
        e.meta.frameTimes.push_back ((double) i / meta.fps);
    }

    e.meta.numFrames = n;
    e.meta.durationSeconds = seconds;
    e.meta.envelope = computeEnvelope (perf.data(), perf.size(), SR);
    eng.history().finishTake (e);
    e.meta.state = TakeMeta::State::review;

    Recorded r;
    r.entry = e;
    r.audio = perf;
    return r;
}

/** Samples where the picture shows the right take and time (labelOk, small error), as a fraction of those with truth. */
double rightFraction (const std::vector<Sample>& s, double from, double to, double tolMs = 80.0)
{
    int n = 0, good = 0;
    for (const auto& x : s)
        if (x.t >= from && x.t < to && x.truth && x.playing)
        {
            ++n;
            good += x.labelOk && std::abs (x.errMs) < tolMs ? 1 : 0;
        }
    return n > 0 ? (double) good / n : 0.0;
}

/** rightFraction for half second comp pieces starting at `first`, leaving out `settle` seconds after each cut. */
double rightAwayFromCuts (const std::vector<Sample>& s, double first, double to, double settle)
{
    int n = 0, good = 0;
    for (const auto& x : s)
        if (x.t >= first && x.t < to && x.truth && x.playing && std::fmod (x.t - first, 0.5) > settle)
        {
            ++n;
            good += x.labelOk && std::abs (x.errMs) < 100.0 ? 1 : 0;
        }
    return n > 0 ? (double) good / n : 0.0;
}

int flips (const std::vector<Sample>& s, double from, double to)
{
    int changes = 0, last = -1;
    for (const auto& x : s)
        if (x.t >= from && x.t < to && x.shown)
        {
            if (last >= 0 && x.label != last && getenv ("GRIDDY_DEBUG"))
                say ("   change at t " + juce::String (x.t, 3) + " host " + juce::String (x.host, 3) + ": " + juce::String (last) + " -> " + juce::String (x.label)
                     + " kind " + juce::String ((int) x.kind) + " status " + juce::String ((int) x.status));
            if (last >= 0 && x.label != last)
                ++changes;
            last = x.label;
        }
    return changes;
}

int countIf (const std::vector<Sample>& s, double from, double to, const std::function<bool (const Sample&)>& f, int* total = nullptr)
{
    int n = 0, hit = 0;
    for (const auto& x : s)
        if (x.t >= from && x.t < to)
        {
            ++n;
            hit += f (x) ? 1 : 0;
        }
    if (total != nullptr)
        *total = n;
    return hit;
}

/** Seconds from `at` until the shown take is `label` and stays so for 3 samples. */
double switchTime (const std::vector<Sample>& s, double at, int label)
{
    int run = 0;
    for (const auto& x : s)
    {
        if (x.t < at)
            continue;
        run = (x.shown && x.label == label) ? run + 1 : 0;
        if (run >= 3)
            return x.t - at - 2 * 0.033;
    }
    return -1.0;
}

// Three takes recorded over the same 5 seconds; the comp plays A, then B, then C, then A again, each where it was recorded.
void compAligned()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (101), 10.0));
    auto& B = w->add (recordTake (*w->engine, 0, 5.0, performance (102), 10.0));
    auto& C = w->add (recordTake (*w->engine, 0, 5.0, performance (103), 10.0));
    keepAndLoad (*w, 0, C);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (A, 10.0, 0.0, 1.5));
    tl.clips[0].push_back (Clip::place (B, 11.5, 1.5, 1.5));
    tl.clips[0].push_back (Clip::place (C, 13.0, 3.0, 1.0));
    tl.clips[0].push_back (Clip::place (A, 14.0, 4.0, 1.0));
    const auto s = observe (*w, tl, { { Event::seek, 0.0, 9.8 }, { Event::play, 0.0 } }, 9.8, 5.6, 0);

    const Recorded* order[] = { &A, &B, &C, &A };
    const double cuts[] = { 0.2, 1.7, 3.2, 4.2 };
    for (int i = 1; i < 4; ++i)
    {
        const auto t = switchTime (s, cuts[i], order[i]->label());
        metric ("comp switch " + juce::String (i), t * 1000.0, "ms");
        check (t >= 0.0 && t < 0.2, "comp cut " + juce::String (i) + " shows the new take within 200 ms (" + juce::String (t, 3) + " s)");
    }

    for (int i = 0; i < 4; ++i)
    {
        const auto from = cuts[i] + 0.25, to = (i < 3 ? cuts[i + 1] : 5.15);
        const auto right = rightFraction (s, from, to);
        check (right > 0.95, "comp piece " + juce::String (i + 1) + " shows its own take in sync (" + juce::String (right * 100.0, 0) + " percent)");
    }

    const auto f = flips (s, 0.3, 5.15);
    check (f == 3, "hard cuts only: the picture changes take exactly 3 times (" + juce::String (f) + ")");
    finish (*w);
}

// Crossfaded comp cuts, as Live draws them between take lanes.
void compCrossfade()
{
    for (double fade : { 0.020, 0.100 })
    {
        auto w = makeWorld (1);
        auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (111), 10.0));
        auto& B = w->add (recordTake (*w->engine, 0, 5.0, performance (112), 10.0));
        keepAndLoad (*w, 0, B);

        Timeline tl (1);
        auto a = Clip::place (A, 10.0, 0.0, 2.5 + fade / 2.0);
        a.fadeOut = fade;
        auto b = Clip::place (B, 12.5 - fade / 2.0, 2.5 - fade / 2.0, 2.5 + fade / 2.0);
        b.fadeIn = fade;
        tl.clips[0].push_back (a);
        tl.clips[0].push_back (b);
        const auto s = observe (*w, tl, { { Event::seek, 0.0, 9.8 }, { Event::play, 0.0 } }, 9.8, 5.4, 0);

        const auto name = juce::String (fade * 1000.0, 0) + " ms crossfade";
        const auto t = switchTime (s, 2.7 - fade / 2.0, B.label());
        metric ("switch after a " + name, t * 1000.0, "ms");
        check (t >= 0.0 && t < 0.25, name + ": the new take shows within 250 ms of the fade (" + juce::String (t, 3) + " s)");
        check (flips (s, 0.3, 5.3) == 1, name + ": one hard cut, no flicker (" + juce::String (flips (s, 0.3, 5.3)) + " changes)");
        check (rightFraction (s, 0.4, 2.6 - fade) > 0.95 && rightFraction (s, 3.0, 5.2) > 0.95, name + ": each side shows its own take in sync");
        finish (*w);
    }
}

// One take recorded while the DAW looped 2 seconds three times: the comp uses pass 1, then pass 3.
void compLoopPasses()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 6.0, performance (121, 6.0), 10.0, false, false, 2.0));
    check (A.entry.meta.hostPieces.size() == 3, "the take knows its 3 passes (" + juce::String ((int) A.entry.meta.hostPieces.size()) + ")");
    keepAndLoad (*w, 0, A);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (A, 10.0, 0.0, 1.0)); // pass 1, first second
    auto p3 = Clip::place (A, 11.0, 5.0, 1.0);               // pass 3, second second: take time 5 at host 11
    tl.clips[0].push_back (p3);
    const auto s = observe (*w, tl, { { Event::loopOn, 0.0, 10.0, 12.0 }, { Event::seek, 0.0, 9.9 }, { Event::play, 0.0 } }, 9.9, 6.3, 0);

    // Every wrap: pass 1 for a second, then pass 3. Frames carry the take time, so the error says which pass is shown.
    double worst = 1.0;
    for (int lap = 0; lap < 3; ++lap)
    {
        const auto base = 0.1 + 2.0 * lap;
        worst = std::min ({ worst, rightFraction (s, base + 0.3, base + 1.0), rightFraction (s, base + 1.25, base + 2.0) });
    }
    check (worst > 0.9, "each piece shows its own pass in sync, lap after lap (worst " + juce::String (worst * 100.0, 0) + " percent)");
    finish (*w);
}

// A piece of take B moved away from where it was recorded, over a stretch where A was recorded.
void compMoved()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (131), 10.0));
    auto& B = w->add (recordTake (*w->engine, 0, 5.0, performance (132), 10.0));
    keepAndLoad (*w, 0, B);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (A, 10.0, 0.0, 2.5));
    tl.clips[0].push_back (Clip::place (B, 12.5, 0.3, 2.5)); // B's 0.3 to 2.8 s, played 2.2 s later than recorded
    const auto s = observe (*w, tl, { { Event::seek, 0.0, 9.8 }, { Event::play, 0.0 } }, 9.8, 5.4, 0);

    check (rightFraction (s, 0.4, 2.7) > 0.95, "the unmoved piece of A plays in sync");
    const auto lock = lockTime (s, 2.7);
    metric ("moved piece found after", lock * 1000.0, "ms");
    check (lock >= 0.0 && lock < 1.0, "the moved piece of B is found within 1 s (" + juce::String (lock, 3) + " s)");
    check (rightFraction (s, 2.7 + std::max (lock, 0.0) + 0.1, 5.2) > 0.95, "and then plays in sync");
    finish (*w);
}

// No clip for a second in the middle of a comp: black, then the next take on its first sound.
void compGap()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (141), 10.0));
    auto& B = w->add (recordTake (*w->engine, 0, 5.0, performance (142), 10.0));
    keepAndLoad (*w, 0, B);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (A, 10.0, 0.0, 2.0));
    tl.clips[0].push_back (Clip::place (B, 13.0, 3.0, 2.0));
    const auto s = observe (*w, tl, { { Event::seek, 0.0, 9.8 }, { Event::play, 0.0 }, { Event::pause, 4.3 } }, 9.8, 5.0, 0);

    int n = 0;
    const auto black = countIf (s, 2.45, 3.15, [] (const Sample& x) { return x.gap && x.black; }, &n);
    check (n > 0 && black == n, "the gap goes black (" + juce::String (black) + "/" + juce::String (n) + ")");
    const auto early = countIf (s, 2.2, 2.33, [] (const Sample& x) { return x.black; });
    check (early == 0, "not before 150 ms of silence");
    const auto t = switchTime (s, 3.2, B.label());
    metric ("gap left after", t * 1000.0, "ms");
    check (t >= 0.0 && t < 0.15, "the next take shows on its first sound (" + juce::String (t, 3) + " s)");
    check (rightFraction (s, 0.4, 2.15) > 0.95 && rightFraction (s, 3.4, 4.25) > 0.95, "both pieces play in sync");
    const auto stoppedBlack = countIf (s, 4.45, 5.0, [] (const Sample& x) { return x.black; });
    check (stoppedBlack == 0, "stopped in a clip: never black");
    finish (*w);
}

// Playing on past the end of the only take: black instead of the last frame.
void compEnd()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (151), 10.0));
    keepAndLoad (*w, 0, A);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (A, 10.0, 0.0, 5.0));
    const auto s = observe (*w, tl, { { Event::seek, 0.0, 13.0 }, { Event::play, 0.0 }, { Event::pause, 3.6 } }, 13.0, 4.4, 0);

    check (rightFraction (s, 0.3, 1.95) > 0.95, "the take plays in sync up to its end");
    int n = 0;
    const auto black = countIf (s, 2.25, 3.55, [] (const Sample& x) { return x.black && x.status == SyncStatus::held; }, &n);
    check (n > 0 && black == n, "after the end it goes black and says held (" + juce::String (black) + "/" + juce::String (n) + ")");
    check (countIf (s, 3.8, 4.4, [] (const Sample& x) { return x.black; }) == 0, "stopped after the end: a frame, not black");
    finish (*w);
}

// Take B was silent from 2 to 3 s (the singer rested in that take). The comp plays A, then B from 2 s.
void compSilentTake()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (161), 10.0));
    auto perfB = performance (162);
    for (size_t i = (size_t) (2.0 * SR); i < (size_t) (3.0 * SR); ++i)
        perfB[i] = 0.0f;
    auto& B = w->add (recordTake (*w->engine, 0, 5.0, perfB, 10.0));
    keepAndLoad (*w, 0, B);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (A, 10.0, 0.0, 2.0));
    tl.clips[0].push_back (Clip::place (B, 12.0, 2.0, 3.0));
    const auto s = observe (*w, tl, { { Event::seek, 0.0, 9.8 }, { Event::play, 0.0 } }, 9.8, 5.3, 0);

    if (getenv ("GRIDDY_DEBUG"))
        for (const auto& x : s)
            if (x.black && x.t > 2.2 && x.t < 5.2)
                say ("   black at t " + juce::String (x.t, 3) + " host " + juce::String (x.host, 3) + " gap " + juce::String ((int) x.gap) + " status " + juce::String ((int) x.status));
    check (countIf (s, 2.2, 5.2, [] (const Sample& x) { return x.black; }) == 0, "never black where a take's own audio is silent");
    const auto right = rightFraction (s, 2.5, 3.15);
    check (right > 0.9, "the silent piece shows the take that was silent there (" + juce::String (right * 100.0, 0) + " percent)");
    check (rightFraction (s, 3.4, 5.1) > 0.95, "and it carries on in sync when its sound comes back");
    finish (*w);
}

// A slot no Griddy owns only follows the position: no black, no comps (the leader feeds it silence).
// The plugin never offers test patterns: they exist for the harness, the smoke test and CI only.
void noTestPatterns()
{
    const auto hasTest = []
    {
        for (const auto& d : griddy::listCaptureDevices())
            if (d.kind == "test" || d.id.startsWith ("test:"))
                return true;
        return false;
    };

    griddy::FrameStore store;
    unsetenv ("GRIDDY_TEST_PATTERNS");
    check (! griddy::testPatternsEnabled() && ! hasTest(), "without GRIDDY_TEST_PATTERNS no test pattern is listed");
    check (griddy::createCaptureSource ("test:1", store) == nullptr, "and a set saved with a test pattern cannot open one");
    check (! griddy::defaultCameraId().startsWith ("test:"), "and the default camera is never a test pattern");

    setenv ("GRIDDY_TEST_PATTERNS", "1", 1);
    check (griddy::testPatternsEnabled() && hasTest(), "with it, the tests get their test patterns");
    check (! griddy::defaultCameraId().startsWith ("test:"), "and the default camera is still never a test pattern");
}

void compUnowned()
{
    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (171), 10.0));
    w->add (recordTake (*w->engine, 0, 5.0, performance (172), 10.0));
    keepAndLoad (*w, 0, A);
    w->engine->setGapsAllowed (0, false);

    Timeline tl (1);
    auto c = Clip::place (A, 10.0, 0.0, 5.0);
    c.gain = 0.0f; // what an unowned slot hears
    tl.clips[0].push_back (c);
    const auto s = observe (*w, tl, { { Event::seek, 0.0, 13.0 }, { Event::play, 0.0 } }, 13.0, 4.0, 0);

    check (countIf (s, 0.2, 4.0, [] (const Sample& x) { return x.black || x.gap; }) == 0, "never black");
    int n = 0;
    const auto held = countIf (s, 2.3, 4.0, [] (const Sample& x) { return x.shown && x.status == SyncStatus::held; }, &n);
    check (n > 0 && held == n, "past the end it holds the last frame, as before (" + juce::String (held) + "/" + juce::String (n) + ")");
    finish (*w);
}

// 9 slots with 20 takes each and comps playing, while 2 slots record: no camera frame may be lost.
void compRecordingLoad()
{
    auto w = makeWorld (9);
    std::vector<std::vector<Recorded*>> takes (9);
    for (int slot = 0; slot < 9; ++slot)
        for (int i = 0; i < 20; ++i)
            takes[(size_t) slot].push_back (&w->add (fakeTake (*w->engine, slot, performance ((unsigned) (1000 + slot * 20 + i), 4.0), 10.0)));
    for (int slot = 0; slot < 9; ++slot)
        w->engine->reloadSlot (slot);

    Timeline tl (9);
    for (int slot = 0; slot < 7; ++slot)
        for (int k = 0; k < 8; ++k)
            tl.clips[(size_t) slot].push_back (Clip::place (*takes[(size_t) slot][(size_t) ((k * 7 + slot) % 20)], 10.0 + 0.5 * k, 0.5 * k, 0.5));

    w->engine->setRecordingActive (true);
    w->engine->setGapsAllowed (7, false);
    w->engine->setGapsAllowed (8, false);
    Recorded r7, r8;
    std::thread t7 ([&] { r7 = recordTake (*w->engine, 7, 4.5, performance (2001), 10.0); });
    std::thread t8 ([&] { r8 = recordTake (*w->engine, 8, 4.5, performance (2002), 10.0); });
    const auto cpu0 = processCpuSeconds();
    const auto s = observe (*w, tl, { { Event::seek, 0.0, 9.9 }, { Event::play, 0.0 } }, 9.9, 4.2, 0);
    const auto cpu = processCpuSeconds() - cpu0;
    t7.join();
    t8.join();
    w->engine->setRecordingActive (false);

    metric ("process CPU while 2 slots record and 7 play comps", cpu / 4.2 * 100.0, "percent of one core");
    check (r7.dropped == 0 && r8.dropped == 0, "both recordings keep every camera frame (dropped " + juce::String ((int) r7.dropped) + " and " + juce::String ((int) r8.dropped) + ")");
    check (r7.entry.meta.numFrames > 100 && r8.entry.meta.numFrames > 100, "both recordings wrote their frames");
    const auto right = rightAwayFromCuts (s, 0.1, 4.1, 0.2);
    metric ("comp right under load, from 200 ms after each cut", right * 100.0, "percent");
    metric ("comp right under load, everything", rightFraction (s, 0.1, 4.1, 100.0) * 100.0, "percent");
    check (right > 0.9, "the comp on slot 1 still shows the right takes under load, from 200 ms after each cut (" + juce::String (right * 100.0, 0) + " percent)");
    finish (*w);
}

// What recognising more takes costs: 9 slots with 1, 5, 10 and 20 takes each, comps playing on every slot.
void compBench()
{
    for (int n : { 1, 5, 10, 20 })
    {
        const auto rss0 = rssMegabytes();
        auto w = makeWorld (9);
        std::vector<std::vector<Recorded*>> takes (9);
        for (int slot = 0; slot < 9; ++slot)
            for (int i = 0; i < n; ++i)
                takes[(size_t) slot].push_back (&w->add (fakeTake (*w->engine, slot, performance ((unsigned) (3000 + slot * 20 + i), 4.0), 10.0)));

        const auto r0 = juce::Time::getMillisecondCounterHiRes();
        for (int slot = 0; slot < 9; ++slot)
            w->engine->reloadSlot (slot);
        const auto reloadMs = (juce::Time::getMillisecondCounterHiRes() - r0) / 9.0;
        juce::Thread::sleep (300);
        const auto rss = rssMegabytes() - rss0;

        Timeline tl (9);
        for (int slot = 0; slot < 9; ++slot)
            for (int k = 0; k < 8; ++k)
                tl.clips[(size_t) slot].push_back (Clip::place (*takes[(size_t) slot][(size_t) ((k * 7 + slot) % n)], 10.0 + 0.5 * k, 0.5 * k, 0.5));

        const auto cpu0 = processCpuSeconds();
        double aligned0 = 0.0;
        for (int slot = 0; slot < 9; ++slot)
            aligned0 += w->engine->alignedCpuSeconds (slot);
        const auto s = observe (*w, tl, { { Event::seek, 0.0, 9.9 }, { Event::play, 0.0 } }, 9.9, 4.0, 0);
        const auto cpu = processCpuSeconds() - cpu0;
        double aligned = -aligned0;
        for (int slot = 0; slot < 9; ++slot)
            aligned += w->engine->alignedCpuSeconds (slot);

        const auto tag = juce::String (n) + (n == 1 ? " take" : " takes") + " per slot, 9 slots";
        metric ("process CPU, " + tag, cpu / 4.0 * 100.0, "percent of one core");
        metric ("comp detection CPU, " + tag, aligned / 4.0 * 100.0, "percent of one core");
        metric ("memory added, " + tag, rss, "MB");
        metric ("slot reload, " + tag, reloadMs, "ms");
        if (n > 1)
        {
            metric ("comp right from 200 ms after each cut, " + tag, rightAwayFromCuts (s, 0.1, 3.9, 0.2) * 100.0, "percent");
            metric ("comp right including the cuts, " + tag, rightFraction (s, 0.1, 3.9, 100.0) * 100.0, "percent");
        }
        finish (*w);
    }
}

//==============================================================================
// Soak and benchmarks

double processCpuSeconds()
{
#if JUCE_LINUX || JUCE_MAC
    rusage u {};
    getrusage (RUSAGE_SELF, &u);
    return (double) u.ru_utime.tv_sec + (double) u.ru_utime.tv_usec * 1.0e-6 + (double) u.ru_stime.tv_sec + (double) u.ru_stime.tv_usec * 1.0e-6;
#else
    return 0.0;
#endif
}

double rssMegabytes()
{
#if JUCE_MAC
    mach_task_basic_info info {};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info (mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t) &info, &count) == KERN_SUCCESS)
        return (double) info.resident_size / 1048576.0;
    return 0.0;
#elif JUCE_LINUX
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
    cfg.exportMov = false;
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

    int camCounter = 0;
    SyntheticCamera cam (cfg.width, cfg.height, [] { return 0; }, [&] (juce::Image, double wall)
    {
        // Every slot's take gets its own label so the soak can tell the slots apart.
        for (auto& r : recs)
            r->pushFrame (makeCodedFrame (cfg.width, cfg.height, r->currentTakeId() & 0xff, camCounter), wall);
        ++camCounter;
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
        {
            juce::JPEGImageFormat fmt;
            fmt.setQuality (0.85f);
            fmt.writeImageToStream (makeCodedFrame (sz.first, sz.second, 1, 1), mos);
        }

        mos.flush();
        const juce::MemoryBlock jpeg (mos.getData(), mos.getDataSize());
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int i = 0; i < 60; ++i)
        {
            auto img = juce::ImageFileFormat::loadFrom (jpeg.getData(), jpeg.getSize());
            if (! img.isValid())
                break;
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

void gpuCompositor()
{
    GpuCompositor gpu;
    if (! gpu.initialise (1280, 720))
    {
        say ("  no OpenGL context here; skipped (the CPU compositor stays in use)");
        return;
    }

    say ("  renderer: " + gpu.description());
    const auto software = gpu.description().containsIgnoreCase ("llvmpipe") || gpu.description().containsIgnoreCase ("software") || gpu.description().containsIgnoreCase ("softpipe");
    metric ("renderer is software", software ? 1.0 : 0.0, "");

    for (auto canvas : { Canvas::hd720, Canvas::hd1080 })
    {
        const auto size = canvasSize (canvas);
        if (! gpu.initialise (size.x, size.y))
        {
            check (false, "GL context for " + juce::String (size.x) + "x" + juce::String (size.y));
            continue;
        }

        for (int slots : { 1, 4, 9 })
        {
            std::vector<std::shared_ptr<const VideoFrame>> frames;
            for (int i = 0; i < slots; ++i)
            {
                auto f = std::make_shared<VideoFrame>();
                f->image = makeCodedFrame (640, 360, i + 1, 77 + i * 5);
                frames.push_back (f);
            }

            const auto cells = computeLayout (slots);
            auto frameFor = [&] (int s) { return frames[(size_t) s]; };
            juce::Image cpu (juce::Image::ARGB, size.x, size.y, true, juce::SoftwareImageType());
            juce::Image gl (juce::Image::ARGB, size.x, size.y, true, juce::SoftwareImageType());
            Compositor ref;
            ref.render (cpu, cells, frameFor);
            gpu.render (gl, cells, frameFor);

            // Compare every channel; bilinear implementations differ by a little at edges.
            double sum = 0.0, worst = 0.0;
            {
                const juce::Image::BitmapData a (cpu, juce::Image::BitmapData::readOnly), b (gl, juce::Image::BitmapData::readOnly);
                for (int y = 0; y < size.y; ++y)
                    for (int x = 0; x < size.x; ++x)
                    {
                        const auto pa = a.getPixelColour (x, y), pb = b.getPixelColour (x, y);
                        const double d = (std::abs ((int) pa.getRed() - (int) pb.getRed()) + std::abs ((int) pa.getGreen() - (int) pb.getGreen()) + std::abs ((int) pa.getBlue() - (int) pb.getBlue())) / 3.0;
                        sum += d;
                        worst = std::max (worst, d);
                    }
            }

            const auto mean = sum / ((double) size.x * size.y);
            say ("  " + juce::String (size.x) + "x" + juce::String (size.y) + ", " + juce::String (slots) + " slots: mean channel difference vs CPU " + juce::String (mean, 2) + " of 255, worst pixel " + juce::String (worst, 0));
            check (mean < 2.5, "GPU picture matches the CPU reference (" + juce::String (size.x) + "x" + juce::String (size.y) + ", " + juce::String (slots) + " slots)");

            // Timing: steady state with frames already uploaded, and with a new frame every call.
            double drawSum = 0, readSum = 0, uploadSum = 0;
            const int N = 30;
            for (int i = 0; i < N; ++i)
            {
                gpu.render (gl, cells, frameFor);
                drawSum += gpu.lastDrawMs();
                readSum += gpu.lastReadbackMs();
            }

            for (int i = 0; i < N; ++i)
            {
                for (auto& f : frames)
                {
                    auto n = std::make_shared<VideoFrame>();
                    n->image = f->image;
                    f = n;
                }
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                gpu.render (gl, cells, frameFor);
                uploadSum += juce::Time::getMillisecondCounterHiRes() - t0;
            }

            const auto name = juce::String (size.x) + "x" + juce::String (size.y) + " " + juce::String (slots) + " slots";
            metric ("GPU draw " + name, drawSum / N, "ms");
            metric ("GPU readback " + name, readSum / N, "ms");
            metric ("GPU total with all frames new " + name, uploadSum / N, "ms");
        }
    }

    gpu.shutdown();
}

// Decodes take.mov's audio with the ffmpeg command line, as another program (a DAW) would see it.
bool movAudio (const juce::File& mov, std::vector<float>& out)
{
    const auto raw = mov.getSiblingFile ("mov-audio.f32");
    const auto cmd = "ffmpeg -v error -y -i \"" + mov.getFullPathName() + "\" -map 0:a:0 -f f32le -ac 1 \"" + raw.getFullPathName() + "\"";
    if (std::system (cmd.toRawUTF8()) != 0)
        return false;

    juce::MemoryBlock mb;
    if (! raw.loadFileAsData (mb))
        return false;

    out.resize (mb.getSize() / sizeof (float));
    std::memcpy (out.data(), mb.getData(), out.size() * sizeof (float));
    raw.deleteFile();
    return true;
}

void gridExport()
{
    if (! gridExportAvailable())
    {
        say ("  no MP4 writer in this build; skipped");
        return;
    }

    const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("griddy_grid_export.mp4");
    file.deleteFile();

    ExportSettings cfg;
    cfg.file = file;
    cfg.width = 1280;
    cfg.height = 720;
    cfg.fps = 30;
    cfg.sampleRate = 48000.0;

    GridExporter ex;
    juce::String error;
    check (ex.start (cfg, error), "export starts " + error);

    juce::Image frame (juce::Image::ARGB, 1280, 720, true, juce::SoftwareImageType());
    const int seconds = 2, fps = 30, perFrame = 48000 / fps;
    std::vector<float> l ((size_t) perFrame), r ((size_t) perFrame);
    double phase = 0.0;

    for (int i = 0; i < seconds * fps; ++i)
    {
        for (int k = 0; k < perFrame; ++k)
        {
            l[(size_t) k] = 0.3f * (float) std::sin (phase);
            r[(size_t) k] = 0.3f * (float) std::sin (phase * 1.5);
            phase += 2.0 * juce::MathConstants<double>::pi * 440.0 / 48000.0;
        }

        ex.pushAudio (l.data(), r.data(), perFrame);

        juce::Graphics g (frame);
        g.fillAll (juce::Colour::fromHSV ((float) i / 60.0f, 0.7f, 0.9f, 1.0f));
        g.setColour (juce::Colours::white);
        g.fillRect (20 + i * 10, 300, 100, 100);
        ex.pushVideo (frame, ex.clockSeconds() - 1.0 / fps);
        juce::Thread::sleep (12);
    }

    check (ex.finish (error), "export finishes " + error);
    check (file.existsAsFile() && file.getSize() > 20000, "the file exists and has content (" + juce::String (file.getSize()) + " bytes)");

    const auto info = probeMedia (file);
    say ("  file: " + juce::String (info.width) + "x" + juce::String (info.height) + " " + info.videoCodec + " " + juce::String (info.videoFrames)
         + " pictures, " + info.audioCodec + " " + juce::String (info.audioChannels) + " ch, picture " + juce::String (info.videoSeconds, 2)
         + " s, sound " + juce::String (info.audioSeconds, 2) + " s, dropped " + juce::String (ex.videoFramesDropped()));
    check (info.ok, "the file reads back " + info.problem);
    check (info.width == 1280 && info.height == 720, "picture size is kept");
    check (info.videoFrames >= 50, "most pictures are in the file");
    check (std::abs (info.audioSeconds - 2.0) < 0.25, "the sound is as long as what was played");
    check (std::abs (info.videoSeconds - 2.0) < 0.35, "the picture is as long as the sound");
    file.deleteFile();
}

void movExport()
{
    if (! movExportAvailable())
    {
        say ("  no take.mov exporter in this build; skipped");
        return;
    }

    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 4.0, performance (301, 4.0), 10.0, true, true));
    check (A.movOk, "take.mov written after the take stops");
    const auto mov = A.entry.folder.getChildFile ("take.mov");
    check (mov.existsAsFile() && mov.getSize() > 10000, "take.mov is on disk (" + juce::String (mov.getSize() / 1024) + " KB)");
    metric ("take.mov size for 4 s", (double) mov.getSize() / 1024.0, "KB");

    // The same file, plainly named, in one flat folder next to the session folders.
    const auto flatName = "Slot 1 Take " + juce::String (A.id()) + ".mov";
    check (A.flat.getFileName() == flatName && A.flat.getParentDirectory().getFileName() == "Takes",
           "plain copy written as Takes/" + flatName + " (got " + A.flat.getFullPathName() + ")");
    check (A.flat.getSize() == mov.getSize(), "the plain copy matches take.mov byte for byte in size");

    // Another program reads it: ffprobe sees one picture stream and one audio stream.
    const auto probeOut = A.entry.folder.getChildFile ("probe.txt");
    const auto cmd = "ffprobe -v error -show_entries stream=codec_name,codec_type,nb_frames,width,height,sample_rate -of csv=p=0 \"" + mov.getFullPathName() + "\" > \"" + probeOut.getFullPathName() + "\"";
    if (std::system (cmd.toRawUTF8()) == 0)
    {
        const auto txt = probeOut.loadFileAsString();
        say ("  ffprobe: " + txt.replace ("\n", " | "));
        check (txt.contains ("mjpeg") && txt.contains ("pcm_s16le"), "ffprobe reads an mjpeg picture stream and a pcm audio stream");
    }

    // Our decoder reads every frame back, and each one is the right picture.
    for (bool proxy : { false, true })
    {
        if (! ffmpegAvailable())
            break;

        auto dec = createFfmpegDecoder();
        check (dec != nullptr && dec->open (A.entry.meta, A.entry.folder, proxy), juce::String ("FFmpeg decoder opens take.mov") + (proxy ? " (proxy size)" : " (full size)"));
        if (dec == nullptr)
            continue;

        check (dec->numFrames() == A.entry.meta.numFrames, "frame count matches (" + juce::String (dec->numFrames()) + " of " + juce::String (A.entry.meta.numFrames) + ")");
        int wrong = 0, label, counter;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const auto n = dec->numFrames();
        for (int i = 0; i < n; ++i)
        {
            const auto img = dec->decode (i);
            if (! decodeCoded (img, label, counter) || label != A.label() || counter != i)
                ++wrong;
        }
        const auto ms = (juce::Time::getMillisecondCounterHiRes() - t0) / std::max (1, n);
        check (wrong == 0, "every frame decodes to the right picture (" + juce::String (wrong) + " wrong)");
        metric (juce::String ("FFmpeg decode ") + (proxy ? "proxy" : "full") + " size", ms, "ms per frame");

        // Random access works too, in any order.
        int badRandom = 0;
        for (int i : { n - 1, 3, n / 2, 0, n - 2, 7 })
        {
            const auto img = dec->decode (i);
            badRandom += (! decodeCoded (img, label, counter) || counter != i) ? 1 : 0;
        }
        check (badRandom == 0, "random access decodes the right frames");
    }

    // The platform decoder (AVFoundation on a Mac) reads the same file, if this build has one.
    if (auto platform = createPlatformDecoder())
    {
        say ("  platform decoder: " + juce::String (platform->name()));
        for (bool proxy : { false, true })
        {
            auto dec = createPlatformDecoder();
            check (dec->open (A.entry.meta, A.entry.folder, proxy), juce::String ("platform decoder opens take.mov") + (proxy ? " (proxy size)" : " (full size)"));
            const auto n = dec->numFrames();
            check (n == A.entry.meta.numFrames, "platform decoder frame count (" + juce::String (n) + " of " + juce::String (A.entry.meta.numFrames) + ")");
            int wrong = 0, label, counter;
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            for (int i = 0; i < n; ++i)
            {
                auto img = dec->decode (i);
                const bool ok = decodeCoded (img, label, counter);
                if ((! ok || label != A.label() || counter != i) && i < 4)
                    say ("    platform frame " + juce::String (i) + ": image " + juce::String (img.getWidth()) + "x" + juce::String (img.getHeight())
                         + (img.isValid() ? juce::String (" px0 ") + img.getPixelAt (5, 5).toDisplayString (true) + " px1 " + img.getPixelAt (img.getWidth() / 2, img.getHeight() / 2).toDisplayString (true) : juce::String (" (invalid)"))
                         + ", decoded " + (ok ? "yes" : "no") + " label " + juce::String (label) + " counter " + juce::String (counter)
                         + " (want label " + juce::String (A.label()) + " counter " + juce::String (i) + ")");
                wrong += (! ok || label != A.label() || counter != i) ? 1 : 0;
            }
            metric (juce::String ("platform decode ") + (proxy ? "proxy" : "full") + " size, sequential", (juce::Time::getMillisecondCounterHiRes() - t0) / std::max (1, n), "ms per frame");
            check (wrong == 0, "platform decoder: every frame is the right picture (" + juce::String (wrong) + " wrong)");

            int badRandom = 0;
            const auto t1 = juce::Time::getMillisecondCounterHiRes();
            for (int i : { n - 1, 3, n / 2, 0, n - 2, 7 })
                badRandom += (! decodeCoded (dec->decode (i), label, counter) || counter != i) ? 1 : 0;
            metric (juce::String ("platform decode ") + (proxy ? "proxy" : "full") + " size, random access", (juce::Time::getMillisecondCounterHiRes() - t1) / 6.0, "ms per frame");
            check (badRandom == 0, "platform decoder: random access is right");
        }
    }

    // The audio inside the file still carries the sync signal: play it as a moved clip and lock to it.
    std::vector<float> movSamples;
    if (movAudio (mov, movSamples))
    {
        check (movSamples.size() >= A.audio.size() - 4096, "audio length matches (" + juce::String ((int) movSamples.size()) + " samples)");
        A.audio = movSamples;
        keepAndLoad (*w, 0, A);
        Timeline tl (1);
        tl.clips[0].push_back (Clip::place (A, 40.0, 1.0, 2.5));
        const auto s = observe (*w, tl, { { Event::seek, 0.0, 39.5 }, { Event::play, 0.0 } }, 39.5, 5.0, 0);
        const auto lock = lockTime (s, 0.5);
        metric ("take.mov audio, moved and trimmed: lock", lock * 1000.0, "ms");
        check (lock >= 0.0 && lock < 0.5, "a take.mov dropped on a track locks by its embedded signal (" + juce::String (lock, 3) + " s)");
        int emb = 0, n = 0;
        for (const auto& x : s)
            if (x.truth && x.t > 1.2)
            {
                ++n;
                emb += x.kind == SyncKind::embedded ? 1 : 0;
            }
        check (n > 0 && emb > n * 0.9, "the embedded source is the one in use (" + juce::String (emb) + "/" + juce::String (n) + ")");
        report ("take.mov clip", errStats (s, 0.5 + std::max (lock, 0.0) + 0.1, 3.0));
    }
    else
    {
        say ("  note: ffmpeg command line not available or could not read take.mov; embedded lock from take.mov audio not checked");
    }

    finish (*w);
}

void movPlayback()
{
    if (! movExportAvailable() || (! ffmpegAvailable() && createPlatformDecoder() == nullptr))
    {
        say ("  no take.mov exporter and decoder in this build; skipped");
        return;
    }

    auto w = makeWorld (1);
    auto& A = w->add (recordTake (*w->engine, 0, 5.0, performance (311), 10.0, false, true));
    check (A.movOk, "take.mov written");

    // Remove the JPEG frames: from here the engine can only play the take from take.mov.
    A.entry.folder.getChildFile ("frames").deleteRecursively();
    A.entry.folder.getChildFile ("proxy").deleteRecursively();
    keepAndLoad (*w, 0, A);

    Timeline tl (1);
    tl.clips[0].push_back (Clip::place (A, 10.0, 0.0, 5.0));
    const auto s = observe (*w, tl, { { Event::seek, 0.0, 9.8 }, { Event::loopOn, 0.0, 10.5, 13.5 }, { Event::play, 0.0 } }, 9.8, 8.0, 0);
    const auto st = errStats (s, 0.5, 7.5);
    report ("played from take.mov only, with a loop", st);
    check (st.badFraction < 0.02, "the picture is right (wrong " + juce::String (st.badFraction * 100, 1) + " percent)");
    check (st.p99 < 50.0, "in sync");
    const auto ps = w->engine->playerStats (0);
    metric ("decode mean from take.mov", ps.decodes > 0 ? ps.decodeMsTotal / (double) ps.decodes : 0.0, "ms");
    finish (*w);
}

griddy::harness::Report hubReport()
{
    return { [] (bool ok, const juce::String& what) { check (ok, what); }, [] (const juce::String& s) { say (s); } };
}

struct Scenario { const char* name; std::function<void()> run; bool inDefaultRun; };
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;

    setenv ("GRIDDY_TEST_PATTERNS", "1", 1); // tests record test pattern cameras; the plugin itself never offers them
    // Nothing the harness runs may write to the real takes folder.
    const auto scratch = juce::File::createTempFile ("griddy-harness-takes");
    setenv ("GRIDDY_TAKES_FOLDER", scratch.getFullPathName().toRawUTF8(), 1);

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
        { "silent_playback", silentPlayback, true },
        { "loop_record", loopRecord, true },
        { "replace_big_review", replaceBigReview, true },
        { "duplicate_x8", duplicateEight, true },
        { "trimmed", trimmedClip, true },
        { "split_moved", splitAndMoved, true },
        { "silence_start", silenceStart, true },
        { "identical_takes", identicalTakes, true },
        { "warped", warpedClip, true },
        { "embedded", embeddedSync, true },
        { "comp_aligned", compAligned, true },
        { "comp_crossfade", compCrossfade, true },
        { "comp_loop_passes", compLoopPasses, true },
        { "comp_moved", compMoved, true },
        { "comp_gap", compGap, true },
        { "comp_end", compEnd, true },
        { "comp_silent_take", compSilentTake, true },
        { "comp_unowned", compUnowned, true },
        { "no_test_patterns", noTestPatterns, true },
        { "comp_recording_load", compRecordingLoad, true },
        { "comp_bench", compBench, true },
        { "bench", bench, true },
        { "gpu", gpuCompositor, true },
        { "mov_export", movExport, true },
        { "grid_export", gridExport, true },
        { "mov_playback", movPlayback, true },
        { "hub_record_play", [] { griddy::harness::hubRecordThenPlay (hubReport()); }, true },
        { "hub_sessions", [] { griddy::harness::hubSessions (hubReport()); }, true },
        { "hub_reset_slot", [] { griddy::harness::hubResetSlot (hubReport()); }, true },
        { "hub_ui_status", [] { griddy::harness::hubUiStatus (hubReport()); }, true },
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

    scratch.deleteRecursively();
    say ("");
    say (failures == 0 ? "HARNESS PASSED" : "HARNESS FAILED (" + juce::String (failures) + " checks)");
    return failures == 0 ? 0 : 1;
}
