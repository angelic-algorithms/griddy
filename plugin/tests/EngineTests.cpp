#include <juce_core/juce_core.h>

#include "capture/TestPatternSource.h"
#include "engine/BeatClock.h"
#include "engine/CollageLayout.h"
#include "engine/Compositor.h"
#include "engine/FrameStore.h"
#include "engine/ImageSequenceRecorder.h"
#include "engine/TransportState.h"

#include <thread>

using namespace griddy;

static TransportSnapshot playingAt (double ppq, double bpm = 120.0, int num = 4, int den = 4, int64_t counter = 1)
{
    TransportSnapshot t;
    t.bpm = bpm;
    t.ppqPosition = ppq;
    t.timeSigNumerator = num;
    t.timeSigDenominator = den;
    t.isPlaying = true;
    t.hostProvidedPosition = true;
    t.updateCounter = counter;
    return t;
}

//==============================================================================
struct LayoutTests : juce::UnitTest
{
    LayoutTests() : UnitTest ("Collage layout", "Griddy") {}

    void runTest() override
    {
        beginTest ("grid shape");
        expect (gridShapeFor (1) == juce::Point<int> (1, 1));
        expect (gridShapeFor (2) == juce::Point<int> (2, 1));
        expect (gridShapeFor (4) == juce::Point<int> (2, 2));
        expect (gridShapeFor (5) == juce::Point<int> (3, 2));
        expect (gridShapeFor (9) == juce::Point<int> (3, 3));

        beginTest ("grid uses every slot once and stays inside the frame");
        for (int n = 1; n <= 9; ++n)
        {
            const auto cells = computeLayout (LayoutMode::grid, n, 0);
            expectEquals ((int) cells.size(), n);

            std::set<int> seen;
            for (const auto& c : cells)
            {
                seen.insert (c.slot);
                expect (juce::Rectangle<float> (0, 0, 1, 1).expanded (1.0e-5f).contains (c.area), "cell outside frame");
            }
            expectEquals ((int) seen.size(), n);
        }

        beginTest ("3 in a 2x2 grid centres the last one");
        {
            const auto cells = computeLayout (LayoutMode::grid, 3, 0);
            expectWithinAbsoluteError (cells[2].area.getCentreX(), 0.5f, 1.0e-5f);
        }

        beginTest ("cut shows one slot, cycling with the step");
        for (long long step = 0; step < 8; ++step)
        {
            const auto cells = computeLayout (LayoutMode::cut, 3, step);
            expectEquals ((int) cells.size(), 1);
            expectEquals (cells[0].slot, (int) (step % 3));
            expect (cells[0].area == juce::Rectangle<float> (0, 0, 1, 1));
        }

        beginTest ("negative steps (count in before bar 1) still wrap into range");
        expectEquals (computeLayout (LayoutMode::cut, 4, -1)[0].slot, 3);

        beginTest ("spotlight features the step's slot, others in the strip");
        {
            const auto cells = computeLayout (LayoutMode::spotlight, 4, 5);
            expectEquals ((int) cells.size(), 4);
            expectEquals (cells[0].slot, 1); // 5 % 4
            expectWithinAbsoluteError (cells[0].area.getWidth(), 0.75f, 1.0e-5f);
            expectEquals (cells[1].slot, 2);
            expectEquals (cells[3].slot, 0);
        }
    }
};

//==============================================================================
struct BeatClockTests : juce::UnitTest
{
    BeatClockTests() : UnitTest ("Beat clock", "Griddy") {}

    void runTest() override
    {
        beginTest ("follows host position while playing");
        {
            BeatClock c;
            c.update (playingAt (8.0), 100.0);
            expectWithinAbsoluteError (c.getPositionInQuarterNotes(), 8.0, 1.0e-9);
            expectEquals (c.getStepIndex (SwitchRate::oneBeat), 8LL);
            expectEquals (c.getStepIndex (SwitchRate::oneBar), 2LL);
            expectEquals (c.getStepIndex (SwitchRate::twoBars), 1LL);
        }

        beginTest ("extrapolates between audio blocks at the current tempo");
        {
            BeatClock c;
            c.update (playingAt (4.0, 120.0, 4, 4, 1), 10.0);
            c.update (playingAt (4.0, 120.0, 4, 4, 1), 10.1); // same block, 100 ms later
            expectWithinAbsoluteError (c.getPositionInQuarterNotes(), 4.2, 1.0e-9); // 2 beats per second
        }

        beginTest ("extrapolation is capped if the audio thread stalls");
        {
            BeatClock c;
            c.update (playingAt (4.0, 120.0, 4, 4, 1), 10.0);
            c.update (playingAt (4.0, 120.0, 4, 4, 1), 15.0);
            expectWithinAbsoluteError (c.getPositionInQuarterNotes(), 4.5, 1.0e-9); // 0.25 s cap
        }

        beginTest ("free runs when stopped, starting from where the DAW stopped");
        {
            BeatClock c;
            c.update (playingAt (16.0), 1.0);
            auto stopped = playingAt (16.0);
            stopped.isPlaying = false;
            c.update (stopped, 1.0);
            c.update (stopped, 2.0);
            expect (! c.isPlaying());
            expectWithinAbsoluteError (c.getPositionInQuarterNotes(), 18.0, 1.0e-9);
        }

        beginTest ("odd time signatures change the bar length");
        {
            BeatClock c;
            c.update (playingAt (7.0, 120.0, 7, 8), 0.0);
            expectWithinAbsoluteError (c.getBarLengthInQuarterNotes(), 3.5, 1.0e-12);
            expectEquals (c.getStepIndex (SwitchRate::oneBar), 2LL);
        }

        beginTest ("a position a hair under the beat from float error counts as the beat");
        {
            BeatClock c;
            c.update (playingAt (3.9999999999), 0.0);
            expectEquals (c.getStepIndex (SwitchRate::oneBeat), 4LL);
        }

        beginTest ("same song position always gives the same step (takes line up)");
        {
            BeatClock a, b;
            a.update (playingAt (33.0), 5.0);
            b.update (playingAt (33.0), 999.0);
            expectEquals (a.getStepIndex (SwitchRate::twoBeats), b.getStepIndex (SwitchRate::twoBeats));
        }
    }
};

//==============================================================================
struct TransportStateTests : juce::UnitTest
{
    TransportStateTests() : UnitTest ("Transport state", "Griddy") {}

    void runTest() override
    {
        beginTest ("round trip");
        {
            TransportState s;
            s.write (playingAt (12.5, 93.0, 3, 4, 7));
            const auto r = s.read();
            expectEquals (r.ppqPosition, 12.5);
            expectEquals (r.bpm, 93.0);
            expectEquals (r.timeSigNumerator, 3);
            expectEquals ((int) r.updateCounter, 7);
            expect (r.isPlaying);
        }

        beginTest ("readers never see a half written snapshot");
        {
            // The writer always writes bpm == ppq. A torn read would see them differ.
            TransportState s;
            s.write (playingAt (0.0, 0.0)); // the default state has bpm 120 and ppq 0, which would look "torn"
            std::atomic<bool> stop { false };
            std::atomic<int> torn { 0 };

            std::thread writer ([&]
            {
                for (int i = 0; ! stop; ++i)
                    s.write (playingAt ((double) i, (double) i));
            });

            for (int i = 0; i < 200000; ++i)
            {
                const auto r = s.read();
                if (! juce::exactlyEqual (r.bpm, r.ppqPosition))
                    ++torn;
            }

            stop = true;
            writer.join();
            expectEquals (torn.load(), 0);
        }
    }
};

//==============================================================================
struct CompositorTests : juce::UnitTest
{
    CompositorTests() : UnitTest ("Compositor", "Griddy") {}

    static std::shared_ptr<const VideoFrame> solid (juce::Colour c, int w = 320, int h = 180)
    {
        auto f = std::make_shared<VideoFrame>();
        f->image = juce::Image (juce::Image::ARGB, w, h, false, juce::SoftwareImageType());
        f->image.clear (f->image.getBounds(), c);
        return f;
    }

    void runTest() override
    {
        const auto red = juce::Colours::red, blue = juce::Colours::blue, green = juce::Colours::lime, yellow = juce::Colours::yellow;

        beginTest ("2x2 grid puts each slot in its quadrant");
        {
            juce::Image out (juce::Image::ARGB, 400, 200, true, juce::SoftwareImageType());
            const std::array<std::shared_ptr<const VideoFrame>, 4> frames { solid (red), solid (blue), solid (green), solid (yellow) };

            Compositor comp;
            comp.render (out, computeLayout (LayoutMode::grid, 4, 0), [&] (int slot) { return frames[(size_t) slot]; });

            expect (out.getPixelAt (100, 50) == red);
            expect (out.getPixelAt (300, 50) == blue);
            expect (out.getPixelAt (100, 150) == green);
            expect (out.getPixelAt (300, 150) == yellow);
            expect (out.getPixelAt (200, 50) != red && out.getPixelAt (200, 50) != blue, "gap missing between cells");
        }

        beginTest ("cut fills the whole frame with the stepped slot");
        {
            juce::Image out (juce::Image::ARGB, 160, 90, true, juce::SoftwareImageType());
            const std::array<std::shared_ptr<const VideoFrame>, 2> frames { solid (red), solid (blue) };

            Compositor comp;
            comp.render (out, computeLayout (LayoutMode::cut, 2, 1), [&] (int slot) { return frames[(size_t) slot]; });
            expect (out.getPixelAt (0, 0) == blue);
            expect (out.getPixelAt (159, 89) == blue);
        }

        beginTest ("missing camera draws a placeholder, not garbage");
        {
            juce::Image out (juce::Image::ARGB, 160, 90, true, juce::SoftwareImageType());
            Compositor comp;
            comp.render (out, computeLayout (LayoutMode::cut, 1, 0), [] (int) { return std::shared_ptr<const VideoFrame>(); });
            expect (out.getPixelAt (2, 2) == juce::Colour (0xff1c1c24));
        }

        beginTest ("cover transform fills the cell and centres the crop");
        {
            // A 4:3 source into a 16:9 cell: width matches, height overflows equally top and bottom.
            const auto t = Compositor::coverTransform ({ 0, 0, 400, 300 }, { 0.0f, 0.0f, 160.0f, 90.0f });
            float x = 0, y = 0;
            t.transformPoint (x, y);
            expectWithinAbsoluteError (x, 0.0f, 1.0e-4f);
            expectWithinAbsoluteError (y, -15.0f, 1.0e-4f); // (120 - 90) / 2 cropped from the top
            float x2 = 400, y2 = 300;
            t.transformPoint (x2, y2);
            expectWithinAbsoluteError (x2, 160.0f, 1.0e-4f);
            expectWithinAbsoluteError (y2, 105.0f, 1.0e-4f);
        }
    }
};

//==============================================================================
struct SourcesAndRecorderTests : juce::UnitTest
{
    SourcesAndRecorderTests() : UnitTest ("Sources and recorder", "Griddy") {}

    void runTest() override
    {
        beginTest ("frame store keeps only the newest frame per source");
        {
            FrameStore store;
            expect (store.latest ("x") == nullptr);
            store.push ("x", juce::Image (juce::Image::ARGB, 2, 2, true), 1.0);
            store.push ("x", juce::Image (juce::Image::ARGB, 4, 4, true), 2.0);
            const auto f = store.latest ("x");
            expect (f != nullptr);
            expectEquals (f->image.getWidth(), 4);
            expectEquals ((int) f->serial, 2);
            store.remove ("x");
            expect (store.latest ("x") == nullptr);
        }

        beginTest ("test pattern produces frames on its own thread");
        {
            FrameStore store;
            TestPatternSource src (store, 2, 160, 90, 60.0);
            juce::String error;
            expect (src.start (error));

            std::shared_ptr<const VideoFrame> frame;
            for (int i = 0; i < 100 && frame == nullptr; ++i)
            {
                juce::Thread::sleep (10);
                frame = store.latest (src.getId());
            }

            expect (frame != nullptr, "no frame after 1 s");
            if (frame != nullptr)
                expectEquals (frame->image.getWidth(), 160);

            src.stop();
            expect (store.latest (src.getId()) == nullptr, "stop() should clear its frame");
        }

        beginTest ("image sequence recorder writes frames and a sidecar with song position");
        {
            const auto tmp = juce::File::createTempFile ("griddy-take");
            TakeInfo info;
            info.startPpq = 16.0; // bar 5 in 4/4
            info.bpm = 96.0;
            info.width = 64;
            info.height = 36;

            ImageSequenceRecorder rec;
            juce::String error;
            expect (rec.start (tmp, info, error), error);

            juce::Image frame (juce::Image::ARGB, 64, 36, true, juce::SoftwareImageType());
            for (int i = 0; i < 3; ++i)
                rec.writeFrame (frame, i);

            const auto folder = rec.finish();
            expectEquals (folder.getNumberOfChildFiles (juce::File::findFiles, "*.jpg"), 3);

            const auto json = juce::JSON::parse (folder.getChildFile ("take.json"));
            expectEquals ((int) json["startBar"], 5);
            expectEquals ((double) json["bpm"], 96.0);
            expectEquals ((int) json["frames"], 3);

            folder.deleteRecursively();
        }
    }
};

static LayoutTests layoutTests;
static BeatClockTests beatClockTests;
static TransportStateTests transportStateTests;
static CompositorTests compositorTests;
static SourcesAndRecorderTests sourcesAndRecorderTests;
