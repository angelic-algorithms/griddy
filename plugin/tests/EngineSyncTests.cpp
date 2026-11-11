#include <juce_core/juce_core.h>

#include "engine/HostClock.h"
#include "engine/SeqLock.h"
#include "engine/SpscRing.h"
#include "engine/SyncController.h"
#include "engine/TakeFormat.h"
#include "engine/TakeHistory.h"
#include "engine/sync/EmbeddedSync.h"
#include "engine/sync/Fingerprint.h"
#include "engine/sync/HostPositionClipSource.h"

#include <random>
#include <thread>

using namespace griddy;

namespace
{
/** Deterministic music like test signal: random notes with harmonics and a little noise. */
std::vector<float> makeMusic (double sr, double seconds, unsigned seed, float level = 1.0f)
{
    std::mt19937 rng (seed);
    std::vector<float> v ((size_t) (sr * seconds), 0.0f);
    size_t pos = 0;

    while (pos < v.size())
    {
        const auto f0 = 110.0 * std::pow (2.0, (double) (rng() % 36) / 12.0);
        const auto len = (size_t) (sr * (0.12 + 0.3 * (double) (rng() % 100) / 100.0));

        for (size_t i = 0; i < len && pos + i < v.size(); ++i)
        {
            const auto t = (double) i / sr;
            const auto env = std::exp (-3.0 * t / ((double) len / sr));
            float s = 0.0f;
            for (int h = 1; h <= 5; ++h)
                s += (float) std::sin (6.283185307 * f0 * h * t) / (float) h;
            v[pos + i] += 0.2f * s * (float) env;
        }

        pos += (size_t) ((double) len * 0.6);
    }

    std::normal_distribution<float> nd (0.0f, 1.0f);
    for (auto& x : v)
        x = level * (x + 0.002f * nd (rng));

    return v;
}
} // namespace

//==============================================================================
struct PrimitiveTests : juce::UnitTest
{
    PrimitiveTests() : UnitTest ("Lock free primitives", "Griddy") {}

    struct Pair { int64_t a, b; };

    void runTest() override
    {
        beginTest ("seqlock never shows a torn value");
        {
            SeqLock<Pair> lock;
            std::atomic<bool> stop { false };
            std::atomic<int> torn { 0 };

            std::thread writer ([&] { for (int64_t i = 1; ! stop; ++i) lock.write ({ i, -i }); });
            for (int i = 0; i < 300000; ++i)
            {
                const auto p = lock.read();
                if (p.a != -p.b)
                    ++torn;
            }
            stop = true;
            writer.join();
            expectEquals (torn.load(), 0);
        }

        beginTest ("spsc ring delivers every item in order and counts drops");
        {
            SpscRing<int> ring (1024);
            std::atomic<bool> done { false };
            int next = 0, bad = 0;

            std::thread producer ([&]
            {
                for (int i = 0; i < 200000;)
                    i += (int) ring.write (&i, 1) > 0 ? 1 : 0;
                done = true;
            });

            while (! done || ring.available() > 0)
            {
                int v;
                if (ring.read (&v, 1) == 1)
                {
                    if (v != next)
                        ++bad;
                    ++next;
                }
            }

            producer.join();
            expectEquals (bad, 0);
            expectEquals (next, 200000);

            SpscRing<int> small (16);
            int block[64] = {};
            expectEquals ((int) small.write (block, 64), 16);
            expectEquals ((int) small.numDropped(), 48);
        }

        beginTest ("wav round trip");
        {
            const auto f = juce::File::createTempFile (".wav");
            WavWriter w;
            expect (w.open (f, 44100.0));
            std::vector<float> a (1000);
            for (size_t i = 0; i < a.size(); ++i)
                a[i] = std::sin ((float) i * 0.1f) * 0.5f;
            w.write (a.data(), 1000);
            w.close();

            std::vector<float> b;
            double sr = 0.0;
            expect (readWav (f, b, sr));
            expectEquals (sr, 44100.0);
            expectEquals ((int) b.size(), 1000);
            expectWithinAbsoluteError (b[500], a[500], 1.0e-4f);
            f.deleteFile();
        }
    }
};

//==============================================================================
struct EmbeddedSyncTests : juce::UnitTest
{
    EmbeddedSyncTests() : UnitTest ("Embedded sync", "Griddy") {}

    void runTest() override
    {
        const double sr = 48000.0;

        beginTest ("every frame is found, with the right tag, counter and position");
        {
            std::vector<float> a ((size_t) (sr * 5.0), 0.0f);
            embedded::Encoder enc (sr, 42);
            enc.mixInto (a.data(), (int) a.size());

            embedded::Decoder dec (sr);
            std::vector<embedded::Detection> out;
            for (size_t i = 0; i < a.size(); i += 480)
                dec.push (a.data() + i, 480, out);

            const auto expected = (int) (5.0 / (48.0 * 120.0 / sr)) - 1;
            expect ((int) out.size() >= expected, "found " + juce::String ((int) out.size()) + " frames, expected about " + juce::String (expected));

            int wrong = 0, worst = 0;
            for (size_t i = 0; i < out.size(); ++i)
            {
                if (out[i].takeTag != 42 || out[i].counter != i)
                    ++wrong;
                worst = std::max (worst, (int) std::abs (out[i].endIndex - out[i].takeSample));
            }

            expectEquals (wrong, 0);
            expect (worst <= 4, "timing off by " + juce::String (worst) + " samples");
            logMessage ("worst timing error " + juce::String (worst) + " samples");
        }

        beginTest ("found under music and hiss, and nothing is found in music alone");
        {
            auto music = makeMusic (sr, 8.0, 11, 0.5f);
            std::mt19937 rng (5);
            std::normal_distribution<float> nd (0.0f, 1.0f);
            for (auto& x : music)
                x += 0.001f * nd (rng);

            auto clean = music;
            embedded::Decoder none (sr);
            std::vector<embedded::Detection> falseHits;
            for (size_t i = 0; i < clean.size(); i += 480)
                none.push (clean.data() + i, 480, falseHits);
            expectEquals ((int) falseHits.size(), 0);

            embedded::Encoder enc (sr, 9);
            enc.mixInto (music.data(), (int) music.size());
            embedded::Decoder dec (sr);
            std::vector<embedded::Detection> out;
            for (size_t i = 0; i < music.size(); i += 480)
                dec.push (music.data() + i, 480, out);

            expect (out.size() > 40, "only " + juce::String ((int) out.size()) + " frames under music");
        }

        beginTest ("a clip cut from the middle still reports the right take position");
        {
            std::vector<float> a ((size_t) (sr * 6.0), 0.0f);
            embedded::Encoder enc (sr, 3);
            enc.mixInto (a.data(), (int) a.size());

            const auto cut = (size_t) (sr * 2.2137);
            embedded::Decoder dec (sr);
            std::vector<embedded::Detection> out;
            dec.push (a.data() + cut, (int) (a.size() - cut), out);
            expect (! out.empty());
            if (! out.empty())
            {
                const auto firstMs = (double) out[0].endIndex / sr * 1000.0;
                logMessage ("first lock after " + juce::String (firstMs, 1) + " ms of audio");
                expect (firstMs < 300.0, "lock took " + juce::String (firstMs, 1) + " ms");
                expect (std::abs (out[0].takeSample - (out[0].endIndex + (int64_t) cut)) <= 4);
            }
        }
    }
};

//==============================================================================
struct FingerprintTests : juce::UnitTest
{
    FingerprintTests() : UnitTest ("Fingerprint", "Griddy") {}

    void runTest() override
    {
        const double sr = 48000.0;
        auto take = makeMusic (sr, 40.0, 7);
        auto idx = std::make_shared<fingerprint::Index> (take.data(), take.size(), sr);
        idx->takeId = 5;

        auto run = [&] (const std::vector<float>& live, double hostStart, double expectedTakeAtHostStart, double& lockMs, double& errMs)
        {
            fingerprint::FingerprintClipSource cs (sr);
            cs.setIndexes ({ idx });
            lockMs = -1.0;
            errMs = 1.0e9;
            const int blk = 512;
            const auto host0 = (int64_t) (hostStart * sr);

            for (size_t i = 0; i + blk <= live.size(); i += blk)
            {
                cs.feed (live.data() + i, blk, host0 + (int64_t) i, true);
                cs.process();
                const auto f = cs.latest();

                if (f.locked)
                {
                    if (lockMs < 0.0)
                        lockMs = (double) i / sr * 1000.0;
                    errMs = std::abs (f.takeSeconds - (f.hostSeconds - hostStart + expectedTakeAtHostStart)) * 1000.0;
                }
            }
        };

        beginTest ("locks fast and exactly on the same audio, any start point");
        for (double startSec : { 0.0, 7.31, 20.0007, 33.9 })
        {
            const auto s0 = (size_t) (startSec * sr);
            std::vector<float> live (take.begin() + (long) s0, take.begin() + (long) s0 + (long) (6 * sr));
            double lockMs, errMs;
            run (live, 100.0, startSec, lockMs, errMs);
            logMessage ("start " + juce::String (startSec) + " s: lock " + juce::String (lockMs, 0) + " ms, error " + juce::String (errMs, 3) + " ms");
            expect (lockMs >= 0.0 && lockMs < 1500.0, "no lock");
            expect (errMs < 2.0, "error " + juce::String (errMs));
        }

        beginTest ("survives gain change, added noise and a different host sample alignment");
        {
            const auto s0 = (size_t) (12.0 * sr) + 17; // not a multiple of the analysis hop
            std::vector<float> live (take.begin() + (long) s0, take.begin() + (long) s0 + (long) (8 * sr));
            std::mt19937 rng (2);
            std::normal_distribution<float> nd (0.0f, 1.0f);
            for (auto& x : live)
                x = 0.4f * x + 0.003f * nd (rng);

            double lockMs, errMs;
            run (live, 50.0, 12.0 + 17.0 / sr, lockMs, errMs);
            logMessage ("noisy: lock " + juce::String (lockMs, 0) + " ms, error " + juce::String (errMs, 3) + " ms");
            expect (lockMs >= 0.0 && lockMs < 3000.0, "no lock");
            expect (errMs < 3.0, "error " + juce::String (errMs));
        }

        beginTest ("unrelated audio never locks");
        {
            const auto other = makeMusic (sr, 8.0, 99);
            double lockMs, errMs;
            run (other, 0.0, 0.0, lockMs, errMs);
            expect (lockMs < 0.0, "false lock at " + juce::String (lockMs, 0) + " ms");
        }

        beginTest ("time stretched audio does not lock (so it can be reported, not shown wrong)");
        {
            // 8 percent faster, the way a warped clip sounds after a tempo change.
            std::vector<float> stretched;
            for (double pos = 10.0 * sr; pos < 18.0 * sr; pos += 1.08)
            {
                const auto i = (size_t) pos;
                const auto frac = (float) (pos - (double) i);
                stretched.push_back (take[i] * (1.0f - frac) + take[i + 1] * frac);
            }

            double lockMs, errMs;
            run (stretched, 0.0, 10.0, lockMs, errMs);
            logMessage ("stretched: lock " + juce::String (lockMs, 0) + " ms, error " + juce::String (errMs, 1) + " ms");
            expect (lockMs < 0.0 || errMs > 20.0, "locked and claimed to be accurate on stretched audio");
        }
    }
};

//==============================================================================
struct TakeHistoryTests : juce::UnitTest
{
    TakeHistoryTests() : UnitTest ("Take history", "Griddy") {}

    static TakeHistory::Entry makeTake (TakeHistory& h, int slot)
    {
        auto e = h.beginTake (slot, TakeMeta {});
        e.meta.numFrames = 1;
        e.meta.durationSeconds = 1.0;
        e.folder.getChildFile ("frames/000000.jpg").replaceWithText ("x");
        h.finishTake (e);
        return e;
    }

    void runTest() override
    {
        const auto root = juce::File::createTempFile ("griddy-history");
        TakeHistory h (root);

        beginTest ("keep makes a take active and archives the previous one");
        auto a = makeTake (h, 0);
        expect (h.inReview (0).has_value());
        expect (! h.active (0).has_value());
        expect (h.keep (0, a.meta.takeId));
        expectEquals ((int) h.active (0)->meta.takeId, (int) a.meta.takeId);

        auto b = makeTake (h, 0);
        expectEquals ((int) h.playable (0).front().meta.takeId, (int) b.meta.takeId); // review first, so it plays
        expect (h.keep (0, b.meta.takeId));
        expectEquals ((int) h.active (0)->meta.takeId, (int) b.meta.takeId);
        expect (a.folder.isDirectory(), "the previous take must stay on disk");

        beginTest ("retake discards the new take and leaves the active one alone");
        auto c = makeTake (h, 0);
        expect (h.retake (0, c.meta.takeId));
        expect (! c.folder.exists());
        expectEquals ((int) h.active (0)->meta.takeId, (int) b.meta.takeId);
        expect (! h.inReview (0).has_value());

        beginTest ("restore swaps an archived take back in");
        expect (h.restore (0, a.meta.takeId));
        expectEquals ((int) h.active (0)->meta.takeId, (int) a.meta.takeId);

        beginTest ("the list shows the newest five and never deletes older takes");
        std::vector<TakeHistory::Entry> many;
        for (int i = 0; i < 7; ++i)
        {
            auto e = makeTake (h, 1);
            h.keep (1, e.meta.takeId);
            many.push_back (e);
        }
        expectEquals ((int) h.listed (1).size(), 5);
        expect (many.front().folder.isDirectory(), "oldest take is still on disk");

        beginTest ("slip is saved with the take");
        expect (h.setSlip (0, a.meta.takeId, 0.035));
        expectWithinAbsoluteError (h.active (0)->meta.slipSeconds, 0.035, 1.0e-9);

        beginTest ("slots are independent");
        expectEquals ((int) h.listed (2).size(), 0);

        root.deleteRecursively();
    }
};

//==============================================================================
struct SyncControllerTests : juce::UnitTest
{
    SyncControllerTests() : UnitTest ("Sync controller", "Griddy") {}

    /** A ClipSource a test can set by hand. */
    struct FakeSource final : ClipSource
    {
        SyncKind k;
        explicit FakeSource (SyncKind kind) : k (kind) {}
        SyncKind kind() const noexcept override { return k; }
        void feed (const float*, int, int64_t, bool) noexcept override {}
        void process() override {}
        ClipFix latest() const noexcept override { return fix.read(); }
        void set (ClipFix f) { f.serial = ++n; fix.write (f); }
        SeqLock<ClipFix> fix;
        uint32_t n = 0;
    };

    static TakeProfile profile (int id, double hostStart, double dur)
    {
        TakeProfile p;
        p.takeId = id;
        p.hostStartSeconds = hostStart;
        p.durationSeconds = dur;
        return p;
    }

    void runTest() override
    {
        const double sr = 48000.0;
        HostClock clock;
        FakeSource emb (SyncKind::embedded), fp (SyncKind::fingerprint);
        HostPositionClipSource prior;
        prior.setSampleRate (sr);
        SyncController ctl (clock, &emb, &fp, &prior);
        ctl.setTakes ({ profile (1, 10.0, 8.0) });
        prior.setTakes ({ { 1, 10.0, 8.0 } });

        float loud[64];
        for (auto& x : loud) x = 0.3f;

        auto feed = [&] (double hostSec, bool playing)
        {
            clock.feed (loud, 64, (int64_t) (hostSec * sr), sr, playing);
            prior.feed (loud, 64, (int64_t) (hostSec * sr), playing);
        };

        beginTest ("with no fixes the host position prior places the take");
        feed (12.0, true);
        auto t = ctl.update (steadyNowSeconds());
        expect (t.status == SyncStatus::prior);
        expectWithinAbsoluteError (t.takeSeconds, 2.0, 0.3);

        beginTest ("a fingerprint fix replaces the prior and moves the clip");
        {
            ClipFix f;
            f.takeId = 1; f.takeSeconds = 5.0; f.hostSeconds = 12.0; f.kind = SyncKind::fingerprint; f.locked = true; f.confidence = 0.9f;
            fp.set (f);
            feed (12.0, true);
            t = ctl.update (steadyNowSeconds());
            expect (t.status == SyncStatus::locked);
            expectWithinAbsoluteError (t.takeSeconds, 5.0, 0.3);
        }

        beginTest ("a single disagreeing fingerprint fix is not believed, a repeated one is");
        {
            ClipFix f;
            f.takeId = 1; f.takeSeconds = 1.0; f.hostSeconds = 12.0; f.kind = SyncKind::fingerprint; f.locked = true;
            fp.set (f);
            feed (12.0, true);
            t = ctl.update (steadyNowSeconds());
            expectWithinAbsoluteError (t.takeSeconds, 5.0, 0.3);

            fp.set (f);
            t = ctl.update (steadyNowSeconds());
            expectWithinAbsoluteError (t.takeSeconds, 1.0, 0.3);
        }

        beginTest ("an embedded fix wins and is believed at once");
        {
            ClipFix f;
            f.takeId = 1; f.takeSeconds = 3.0; f.hostSeconds = 12.0; f.kind = SyncKind::embedded; f.locked = true;
            emb.set (f);
            feed (12.0, true);
            t = ctl.update (steadyNowSeconds());
            expectWithinAbsoluteError (t.takeSeconds, 3.0, 0.3);
            expect (t.kind == SyncKind::embedded);
        }

        beginTest ("stopping freezes the position; seeking away keeps the clip placement");
        {
            feed (12.0, false);
            t = ctl.update (steadyNowSeconds());
            expect (t.status == SyncStatus::stopped);
            const auto frozen = t.takeSeconds;
            juce::Thread::sleep (50);
            expectWithinAbsoluteError (ctl.update (steadyNowSeconds()).takeSeconds, frozen, 1.0e-9);

            feed (14.0, false); // seek forward 2 s
            expectWithinAbsoluteError (ctl.update (steadyNowSeconds()).takeSeconds, frozen + 2.0, 0.01);
        }

        beginTest ("a loop wrap in the host leaves the placement alone, so the clip repeats with the loop");
        {
            feed (13.0, true);
            const auto before = ctl.update (steadyNowSeconds()).takeSeconds;
            feed (12.0, true); // loop jumped back one second
            const auto after = ctl.update (steadyNowSeconds()).takeSeconds;
            expectWithinAbsoluteError (before - after, 1.0, 0.3);
        }

        beginTest ("past the end of the take: wraps if sound continues, holds if it stops");
        {
            // placement: take 3.0 at host 12.0 => offset -9.0; take is 8.0 long, so host 20.5 is past the end
            feed (20.5, true);
            t = ctl.update (steadyNowSeconds());
            expect (t.predictedWrap == 1);
            expect (t.takeSeconds >= 0.0 && t.takeSeconds <= 8.0);

            float quiet[64] = {};
            clock.feed (quiet, 64, (int64_t) (20.5 * sr), sr, true);
            t = ctl.update (steadyNowSeconds());
            expect (t.status == SyncStatus::held);
        }
    }
};

static PrimitiveTests primitiveTests;
static EmbeddedSyncTests embeddedSyncTests;
static FingerprintTests fingerprintTests;
static TakeHistoryTests takeHistoryTests;
static SyncControllerTests syncControllerTests;
