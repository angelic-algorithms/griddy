// A tiny fake DAW. Loads the built Griddy VST3 the way a real host would and checks
// the things that only show up once the plugin is wrapped:
//   * the host can scan and open it
//   * the parameters a DAW would show for automation are there
//   * the audio path stays bit exact while the transport moves
//   * state saved into a "project" restores into a new instance
//   * the WebView editor opens and closes without crashing
//
// Usage: GriddyHostSmokeTest /path/to/Griddy.vst3   (run under xvfb-run on a headless Linux box)
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <filesystem>

using namespace juce;

namespace
{
int failures = 0;

void check (bool ok, const String& what)
{
    std::cout << (ok ? "  ok    " : "  FAIL  ") << what << std::endl;
    if (! ok)
        ++failures;
}

/** Pretends to be a DAW transport that is playing and recording at 100 BPM. */
struct FakePlayHead final : AudioPlayHead
{
    double ppq = 8.0; // bar 3 in 4/4
    int64_t samples = 0;
    bool playing = true, recording = true;

    Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setBpm (100.0);
        info.setPpqPosition (ppq);
        info.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
        info.setTimeSignature (TimeSignature { 4, 4 });
        info.setIsPlaying (playing);
        info.setIsRecording (recording);
        info.setTimeInSamples (samples);
        return info;
    }
};

void pumpMessages (int ms)
{
    const auto end = Time::getMillisecondCounter() + (uint32) ms;
    while (Time::getMillisecondCounter() < end)
        MessageManager::getInstance()->runDispatchLoopUntil (20);
}

std::unique_ptr<AudioPluginInstance> load (AudioPluginFormatManager& fm, const PluginDescription& d)
{
    String error;
    auto instance = fm.createPluginInstance (d, 48000.0, 512, error);
    check (instance != nullptr, "createPluginInstance " + error);
    return instance;
}

/** Runs the audio callback for `seconds` of audio at 48 kHz, advancing the fake transport. */
void runAudio (AudioPluginInstance& p, FakePlayHead& head, double seconds, bool noise = false)
{
    Random rng (7);
    AudioBuffer<float> buffer (2, 512);
    MidiBuffer midi;
    const auto blocks = (int) (seconds * 48000.0 / 512.0);

    for (int i = 0; i < blocks; ++i)
    {
        buffer.clear();
        if (noise)
            for (int ch = 0; ch < 2; ++ch)
                for (int n = 0; n < 512; ++n)
                    buffer.setSample (ch, n, 0.3f * (rng.nextFloat() * 2.0f - 1.0f));
        else
            buffer.setSample (0, 0, 0.5f);

        p.processBlock (buffer, midi);
        head.ppq += 512.0 / 48000.0 * (100.0 / 60.0);
        head.samples += 512;

        // Real time pacing, so the 30 fps video engine sees the transport move.
        if (i % 4 == 0)
            pumpMessages (40);
    }

    // Audio should pass through untouched.
    if (! noise)
        check (std::abs (buffer.getSample (0, 0) - 0.5f) < 1.0e-6f, "audio passes through");
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: GriddyHostSmokeTest /path/to/Griddy.vst3" << std::endl;
        return 2;
    }

    ScopedJuceInitialiser_GUI init;
    AudioPluginFormatManager fm;
    addDefaultFormatsToManager (fm);

    OwnedArray<PluginDescription> types;
    for (auto* format : fm.getFormats())
        if (format->getName() == "VST3")
            format->findAllTypesForFile (types, String (std::filesystem::weakly_canonical (argv[1]).string()));

    check (types.size() == 1, "host finds exactly one plugin in the bundle");
    if (types.isEmpty())
        return 1;

    std::cout << "Loaded: " << types[0]->name << " by " << types[0]->manufacturerName << std::endl;

    MemoryBlock savedState;

    {
        auto plugin = load (fm, *types[0]);
        if (plugin == nullptr)
            return 1;

        StringArray names;
        for (auto* param : plugin->getParameters())
            names.add (param->getName (64));
        std::cout << "Parameters: " << names.joinIntoString (", ") << std::endl;

        for (const auto* expected : { "Cameras", "Record arm", "Record when" })
            check (names.contains (expected), String ("parameter '") + expected + "' exposed to the host");

        FakePlayHead head;
        plugin->setPlayHead (&head);
        plugin->prepareToPlay (48000.0, 512);

        for (auto* param : plugin->getParameters())
            if (param->getName (64) == "Cameras")
                param->setValueNotifyingHost (1.0f); // 9 slots

        // Play for ~2 seconds, then stop the transport.
        runAudio (*plugin, head, 2.0);
        head.playing = head.recording = false;
        runAudio (*plugin, head, 0.5);

        File::getSpecialLocation (File::userMoviesDirectory).getChildFile ("Griddy").getChildFile ("Sessions").deleteRecursively();

        // Record a take: arm, play with the DAW recording for a few seconds, then stop.
        for (auto* param : plugin->getParameters())
            if (param->getName (64) == "Record arm")
                param->setValueNotifyingHost (1.0f);

        head.playing = head.recording = true;
        runAudio (*plugin, head, 4.0, true);
        head.playing = head.recording = false;
        runAudio (*plugin, head, 0.5);
        pumpMessages (5000); // the take is finished and analysed on worker threads

        {
            // The default output folder; the VST3 wrapper hides the plugin's own state, so look on disk.
            const auto sessions = File::getSpecialLocation (File::userMoviesDirectory).getChildFile ("Griddy").getChildFile ("Sessions");
            check (sessions.isDirectory(), "the plugin made a sessions folder");
            const auto slotFolder = sessions;
            Array<File> takes;
            slotFolder.findChildFiles (takes, File::findDirectories, true, "*.gtake");
            check (takes.size() == 1, "one take was recorded (" + String (takes.size()) + " found)");

            if (takes.size() == 1)
            {
                const auto meta = JSON::parse (takes[0].getChildFile ("take.json"));
                const auto state = meta.getProperty ("state", {}).toString();
                const auto frames = meta.getProperty ("frameTimes", {}).getArray() != nullptr ? meta.getProperty ("frameTimes", {}).getArray()->size() : 0;
                const auto seconds = (double) meta.getProperty ("durationSeconds", 0.0);
                std::cout << "  take: state " << state << ", " << frames << " frames, " << seconds << " s" << std::endl;
                check (state == "review", "the take is waiting for review");
                check (seconds > 3.5 && seconds < 4.6, "the reference audio is about as long as the recording");
                check (frames > 40 && frames < 150, "camera frames were recorded (" + String (frames) + " in about 4 s; a loaded CI machine can fall well short of 30 fps)");
                check (takes[0].getChildFile ("audio.wav").existsAsFile(), "the take has its reference audio");
            }
        }

        // Editor: create the WebView UI, let it load, then tear it down.
        {
            std::unique_ptr<AudioProcessorEditor> editor (plugin->createEditorAndMakeActive());
            check (editor != nullptr, "editor opens");

            if (editor != nullptr)
            {
                editor->addToDesktop (0);
                editor->setVisible (true);
                pumpMessages (1500);
                check (editor->getWidth() >= 720 && editor->getHeight() >= 480, "editor has a sensible size");
            }
        }

        plugin->releaseResources();
        plugin->getStateInformation (savedState);
    }

    // A new instance restores what the "project" saved.
    {
        auto plugin = load (fm, *types[0]);
        if (plugin == nullptr)
            return 1;

        plugin->setStateInformation (savedState.getData(), (int) savedState.getSize());

        for (auto* param : plugin->getParameters())
            if (param->getName (64) == "Cameras")
                check (param->getCurrentValueAsText() == "9", "slot count restored from saved state");
    }

    std::cout << (failures == 0 ? "HOST SMOKE TEST PASSED" : "HOST SMOKE TEST FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
