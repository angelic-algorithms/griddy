#include "PlaybackEngine.h"

#include <algorithm>
#include <cmath>

namespace griddy
{

PlaybackEngine::Slot::Slot (double sr, const SlotPlayer::Config& pc)
    : embedded (sr),
      fingerprint (sr),
      controller (clock, &embedded, &fingerprint, &prior),
      player (controller, pc)
{
    prior.setSampleRate (sr);
}

namespace
{
struct LoopThread final : juce::Thread
{
    LoopThread (const char* name, std::function<void()> f) : juce::Thread (name), body (std::move (f)) {}
    void run() override { body(); }
    std::function<void()> body;
};
} // namespace

PlaybackEngine::PlaybackEngine (Config c) : config (std::move (c)), takeHistory (config.takesRoot)
{
    const auto size = canvasSize (config.canvas);

    // Tile sized proxies are the cheap path for a grid; only a big single tile wants the full frames.
    const auto shape = gridShapeFor (config.numSlots);
    config.player.useProxy = size.x / shape.x <= 800;

    for (int i = 0; i < config.numSlots; ++i)
        slots.push_back (std::make_unique<Slot> (config.sampleRate, config.player));

    output = juce::Image (juce::Image::ARGB, size.x, size.y, true, juce::SoftwareImageType());
}

PlaybackEngine::~PlaybackEngine() { stop(); }

void PlaybackEngine::start()
{
    if (running.exchange (true))
        return;

    for (auto& s : slots)
        s->player.start();

    syncThread = std::make_unique<LoopThread> ("Griddy sync worker", [this] { syncLoop(); });
    syncThread->startThread (juce::Thread::Priority::high);

    if (config.runRenderThread)
    {
        renderThread = std::make_unique<LoopThread> ("Griddy render", [this] { renderLoop(); });
        renderThread->startThread (juce::Thread::Priority::high);
    }
}

void PlaybackEngine::stop()
{
    if (! running.exchange (false))
        return;

    if (renderThread != nullptr)
        renderThread->stopThread (3000);
    if (syncThread != nullptr)
        syncThread->stopThread (3000);

    renderThread.reset();
    syncThread.reset();

    for (auto& s : slots)
        s->player.stop();
}

void PlaybackEngine::feedSlotAudio (int slot, const float* mono, int n, int64_t hostSample, bool playing) noexcept
{
    auto& s = *slots[(size_t) slot];
    s.clock.feed (mono, n, hostSample, config.sampleRate, playing);
    s.embedded.feed (mono, n, hostSample, playing);
    s.fingerprint.feed (mono, n, hostSample, playing);
    s.prior.feed (mono, n, hostSample, playing);
}

void PlaybackEngine::reloadSlot (int slot)
{
    auto& s = *slots[(size_t) slot];
    const auto playable = takeHistory.playable (slot);

    std::vector<std::shared_ptr<const fingerprint::Index>> indexes;
    std::vector<TakeProfile> profiles;
    std::vector<TakeSpan> spans;
    std::map<int32_t, std::shared_ptr<const fingerprint::Index>> keep;

    for (const auto& e : playable)
    {
        std::shared_ptr<const fingerprint::Index> idx;

        if (auto it = s.indexCache.find (e.meta.takeId); it != s.indexCache.end())
        {
            idx = it->second;
        }
        else
        {
            std::vector<float> audio;
            double sr = 0.0;

            if (readWav (e.folder.getChildFile ("audio.wav"), audio, sr) && ! audio.empty())
            {
                auto built = std::make_shared<fingerprint::Index> (audio.data(), audio.size(), sr);
                built->takeId = e.meta.takeId;
                idx = built;
            }
        }

        if (idx != nullptr)
        {
            indexes.push_back (idx);
            keep[e.meta.takeId] = idx;
        }

        TakeProfile p;
        p.takeId = e.meta.takeId;
        p.hostStartSeconds = e.meta.hostStartSeconds();
        p.durationSeconds = e.meta.durationSeconds;
        p.recordedBpm = e.meta.bpm;
        p.envelope = std::make_shared<const std::vector<float>> (e.meta.envelope);
        profiles.push_back (std::move (p));

        spans.push_back ({ e.meta.takeId, e.meta.hostStartSeconds(), e.meta.durationSeconds });
    }

    s.indexCache = std::move (keep);
    s.fingerprint.setIndexes (std::move (indexes));
    s.prior.setTakes (std::move (spans));
    s.controller.setTakes (std::move (profiles));
    s.player.setTakes (playable);
}

void PlaybackEngine::syncLoop()
{
    while (! juce::Thread::currentThreadShouldExit())
    {
        for (auto& s : slots)
        {
            s->embedded.process();
            s->fingerprint.process();
        }

        juce::Thread::sleep (3);
    }
}

bool PlaybackEngine::enableGpuCompositor()
{
    const auto size = canvasSize (config.canvas);
    return gpu.initialise (size.x, size.y);
}

juce::Image PlaybackEngine::renderOnce()
{
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto cells = computeLayout (config.numSlots);

    std::vector<std::shared_ptr<const ShownFrame>> frames ((size_t) config.numSlots);
    for (int i = 0; i < config.numSlots; ++i)
        frames[(size_t) i] = slots[(size_t) i]->player.shown();

    {
        const std::scoped_lock sl (outputLock);
        auto frameFor = [&] (int slot) -> std::shared_ptr<const VideoFrame>
        {
            const auto& f = frames[(size_t) slot];
            return f != nullptr ? f->frame : nullptr;
        };

        if (gpu.available())
            gpu.render (output, cells, frameFor);
        else
            compositor.render (output, cells, frameFor);
    }

    // Measure how far each shown picture is from where the audio is, right now.
    const auto wall = steadyNowSeconds();
    for (int i = 0; i < config.numSlots; ++i)
    {
        auto& s = *slots[(size_t) i];
        const auto& f = frames[(size_t) i];

        if (f == nullptr || f->frame == nullptr)
        {
            s.lastShownIndex = -1;
            continue;
        }

        if (f->status == SyncStatus::locked || f->status == SyncStatus::prior)
        {
            const auto target = f->targetSeconds + (f->playing ? wall - f->publishWall : 0.0);
            {
                const std::scoped_lock sl (statsLock);
                s.driftMs.push_back ((float) (std::abs (f->frameTakeSeconds - target) * 1000.0));
                if (s.driftMs.size() > 400000)
                    s.driftMs.erase (s.driftMs.begin(), s.driftMs.begin() + 200000);
            }

            // A picture that advanced by a different number of frames than the clock did is a stutter.
            if (f->playing && s.lastShownTake == f->takeId && s.lastShownIndex >= 0)
            {
                const auto clockFrames = (target - s.lastShownTarget) * 30.0;
                const auto pictureFrames = (double) (f->frameIndex - s.lastShownIndex);
                if (clockFrames > -0.5 && clockFrames < 3.0 && std::abs (pictureFrames - clockFrames) > 1.5)
                    ++s.skipped;
            }

            s.lastShownTarget = target;
        }

        s.lastShownIndex = f->frameIndex;
        s.lastShownTake = f->takeId;
    }

    const auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
    {
        const std::scoped_lock sl (statsLock);
        ++render.frames;
        renderMsSum += ms;
        render.renderMsMean = renderMsSum / (double) render.frames;
        render.renderMsMax = std::max (render.renderMsMax, ms);
    }

    return latestOutput();
}

juce::Image PlaybackEngine::latestOutput() const
{
    const std::scoped_lock sl (outputLock);
    return output;
}

void PlaybackEngine::renderLoop()
{
    const auto period = 1.0 / 30.0;
    auto next = steadyNowSeconds();

    while (! juce::Thread::currentThreadShouldExit())
    {
        const auto now = steadyNowSeconds();

        if (now - next > period)
        {
            const std::scoped_lock sl (statsLock);
            ++render.lateFrames;
        }

        renderOnce();

        next += period;
        const auto waitMs = (int) std::floor ((next - steadyNowSeconds()) * 1000.0);

        if (waitMs > 0)
            juce::Thread::sleep (waitMs);
        else if (waitMs < -250)
            next = steadyNowSeconds();
    }
}

SlotDriftStats PlaybackEngine::driftStats (int slot) const
{
    const auto& s = *slots[(size_t) slot];
    SlotDriftStats out;
    std::vector<float> v;
    {
        const std::scoped_lock sl (statsLock);
        v = s.driftMs;
    }
    out.skippedFrames = s.skipped;
    out.samples = v.size();

    if (v.empty())
        return out;

    std::sort (v.begin(), v.end());
    double sum = 0.0;
    for (auto x : v)
        sum += x;

    out.meanMs = sum / (double) v.size();
    out.p50Ms = v[v.size() / 2];
    out.p99Ms = v[(size_t) ((double) v.size() * 0.99)];
    out.maxMs = v.back();
    return out;
}

RenderStats PlaybackEngine::renderStats() const
{
    const std::scoped_lock sl (statsLock);
    return render;
}

} // namespace griddy
