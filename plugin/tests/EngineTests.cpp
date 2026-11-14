#include <juce_core/juce_core.h>

#include "capture/TestPatternSource.h"
#include "engine/BeatClock.h"
#include "engine/CollageLayout.h"
#include "engine/Compositor.h"
#include "engine/FrameStore.h"
#include "engine/HostClock.h"
#include "engine/TransportState.h"

#include <set>
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
            const auto cells = computeLayout (n);
            expectEquals ((int) cells.size(), n);

            std::set<int> seen;
            for (const auto& c : cells)
            {
                seen.insert (c.slot);
                expect (juce::Rectangle<float> (0, 0, 1, 1).expanded (1.0e-5f).contains (c.area), "cell outside frame");
            }
            expectEquals ((int) seen.size(), n);
        }

        beginTest ("every template covers the frame exactly, with no overlap, for 2 to 9 slots");
        for (int t = 0; t < numTemplates; ++t)
            for (int n = 2; n <= 9; ++n)
            {
                const auto shape = (Template) t;
                const auto cells = computeLayout (n, shape);
                expectEquals ((int) cells.size(), n);

                std::set<int> seen;
                float area = 0.0f;
                for (size_t i = 0; i < cells.size(); ++i)
                {
                    seen.insert (cells[i].slot);
                    area += cells[i].area.getWidth() * cells[i].area.getHeight();
                    expect (juce::Rectangle<float> (0, 0, 1, 1).expanded (1.0e-5f).contains (cells[i].area), "cell outside frame");

                    for (size_t j = i + 1; j < cells.size(); ++j)
                        expect (cells[i].area.reduced (1.0e-4f).getIntersection (cells[j].area.reduced (1.0e-4f)).isEmpty(), "cells overlap");
                }

                expectEquals ((int) seen.size(), n);
                if (shape != Template::grid)
                    expectWithinAbsoluteError (area, 1.0f, 1.0e-4f);
            }

        beginTest ("hero shapes give cell 0 the larger area");
        for (auto shape : { Template::heroLeft, Template::heroTop })
            for (int n = 2; n <= 9; ++n)
            {
                const auto cells = computeLayout (n, shape);
                for (size_t i = 1; i < cells.size(); ++i)
                    expect (cells[0].area.getWidth() * cells[0].area.getHeight() > cells[i].area.getWidth() * cells[i].area.getHeight());
            }

        beginTest ("a single slot fills the whole frame");
        expect (computeLayout (1)[0].area == juce::Rectangle<float> (0, 0, 1, 1));

        beginTest ("3 in a 2x2 grid centres the last one");
        {
            const auto cells = computeLayout (3);
            expectWithinAbsoluteError (cells[2].area.getCentreX(), 0.5f, 1.0e-5f);
        }

        beginTest ("every template covers the frame exactly, with no overlap, and has its panel count");
        {
            const int panels[] = { 1, 2, 2, 3, 3, 4, 4, 5, 6, 6, 7, 8, 9 };
            for (int p = 0; p < numPresets; ++p)
            {
                const auto cells = presetLayout (p);
                expectEquals ((int) cells.size(), panels[p], presetAt (p).id);
                expectEquals (presetAt (p).panels, panels[p]);

                float area = 0.0f;
                for (size_t i = 0; i < cells.size(); ++i)
                {
                    expectEquals (cells[i].slot, (int) i);
                    area += cells[i].area.getWidth() * cells[i].area.getHeight();
                    expect (juce::Rectangle<float> (0, 0, 1, 1).expanded (1.0e-5f).contains (cells[i].area), "cell outside frame");
                    for (size_t j = i + 1; j < cells.size(); ++j)
                        expect (cells[i].area.reduced (1.0e-4f).getIntersection (cells[j].area.reduced (1.0e-4f)).isEmpty(), "cells overlap");
                }
                expectWithinAbsoluteError (area, 1.0f, 1.0e-4f);
            }
        }

        beginTest ("templates match the design's cells");
        {
            // 1+2: a big cell two thirds wide on the left, two stacked on the right.
            const auto c = presetLayout (presetIndex ("1p2"));
            expect (c[0].area == juce::Rectangle<float> (0.0f, 0.0f, 2.0f / 3.0f, 1.0f));
            expect (c[2].area == juce::Rectangle<float> (2.0f / 3.0f, 0.5f, 1.0f / 3.0f, 0.5f));
            // 3+4: three on top, four below.
            const auto d = presetLayout (presetIndex ("3p4"));
            expectWithinAbsoluteError (d[3].area.getWidth(), 0.25f, 1.0e-5f);
            expectWithinAbsoluteError (d[0].area.getWidth(), 1.0f / 3.0f, 1.0e-5f);
        }

        beginTest ("every slot count has a default template, and old layouts find one");
        for (int n = 1; n <= 9; ++n)
        {
            expectEquals (presetAt (defaultPresetFor (n)).panels, n);
            for (int t = 0; t < numTemplates; ++t)
                expectEquals (presetAt (presetForTemplate ((Template) t, n)).panels, n);
        }
        expectEquals (presetForTemplate (Template::heroLeft, 3), presetIndex ("1p2"));
        expectEquals (presetForTemplate (Template::stack, 3), presetIndex ("3s"));

        beginTest ("a framed cell keeps one frame width around the edge and between cells, like the design's CSS grid");
        {
            // 300 x 100 picture, frame 10: three columns of (300 - 4 * 10) / 3 wide, starting at 10.
            const auto a = framedCellArea ({ 0.0f, 0.0f, 1.0f / 3.0f, 1.0f }, 300.0f, 100.0f, 10.0f);
            expectWithinAbsoluteError (a.getX(), 10.0f, 1.0e-3f);
            expectWithinAbsoluteError (a.getWidth(), 260.0f / 3.0f, 1.0e-3f);
            expectWithinAbsoluteError (a.getY(), 10.0f, 1.0e-3f);
            expectWithinAbsoluteError (a.getHeight(), 80.0f, 1.0e-3f);
            // Spanning two columns covers the gap between them.
            const auto b = framedCellArea ({ 0.0f, 0.0f, 2.0f / 3.0f, 1.0f }, 300.0f, 100.0f, 10.0f);
            expectWithinAbsoluteError (b.getWidth(), 2.0f * 260.0f / 3.0f + 10.0f, 1.0e-3f);
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
            comp.render (out, computeLayout (4), [&] (int slot) { return frames[(size_t) slot]; });

            expect (out.getPixelAt (100, 50) == red);
            expect (out.getPixelAt (300, 50) == blue);
            expect (out.getPixelAt (100, 150) == green);
            expect (out.getPixelAt (300, 150) == yellow);
            expect (out.getPixelAt (200, 50) != red && out.getPixelAt (200, 50) != blue, "gap missing between cells");
        }

        beginTest ("a framed picture has the frame colour round the edge and rounded cell corners");
        {
            juce::Image out (juce::Image::ARGB, 400, 200, true, juce::SoftwareImageType());
            Compositor comp;
            comp.setFramed (true);
            comp.setGapPixels (10.0f);
            comp.setCornerRadius (30.0f);
            comp.setBackground (0xffff6a1au);
            comp.render (out, presetLayout (presetIndex ("1x1")), [&] (int) { return solid (blue); });

            const auto frame = juce::Colour (0xffff6a1au);
            expect (out.getPixelAt (5, 100) == frame, "frame round the edge");
            expect (out.getPixelAt (200, 100) == blue);
            expect (out.getPixelAt (12, 12) == frame, "corner is rounded off");
            expect (out.getPixelAt (40, 100) == blue, "edge of the cell away from the corner is picture");
        }

        beginTest ("missing camera draws a placeholder, not garbage");
        {
            juce::Image out (juce::Image::ARGB, 160, 90, true, juce::SoftwareImageType());
            Compositor comp;
            comp.render (out, computeLayout (1), [] (int) { return std::shared_ptr<const VideoFrame>(); });
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
struct SourcesTests : juce::UnitTest
{
    SourcesTests() : UnitTest ("Sources", "Griddy") {}

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
            {
                expectEquals (frame->image.getWidth(), 160);

                // Frames are stamped on the recorder's clock; another clock drifts by however long the Mac slept.
                expect (std::abs (frame->captureTimeSeconds - griddy::steadyNowSeconds()) < 1.0, "capture time is on steadyNowSeconds()");
            }

            src.stop();
            expect (store.latest (src.getId()) == nullptr, "stop() should clear its frame");
        }
    }
};

static LayoutTests layoutTests;
static BeatClockTests beatClockTests;
static TransportStateTests transportStateTests;
static CompositorTests compositorTests;
static SourcesTests sourcesAndRecorderTests;
