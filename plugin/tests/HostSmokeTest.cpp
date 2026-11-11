// A tiny fake DAW. Loads the built Griddy VST3 the way a real host would and checks
// the things that only show up once the plugin is wrapped:
//   * the host can scan and open it
//   * the parameters a DAW would show for automation are there
//   * transport from the host reaches the video engine (via a recorded take)
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
void runAudio (AudioPluginInstance& p, FakePlayHead& head, double seconds)
{
    AudioBuffer<float> buffer (2, 512);
    MidiBuffer midi;
    const auto blocks = (int) (seconds * 48000.0 / 512.0);

    for (int i = 0; i < blocks; ++i)
    {
        buffer.clear();
        buffer.setSample (0, 0, 0.5f);
        p.processBlock (buffer, midi);
        head.ppq += 512.0 / 48000.0 * (100.0 / 60.0);

        // Real time pacing, so the 30 fps video engine sees the transport move.
        if (i % 4 == 0)
            pumpMessages (40);
    }

    // Audio should pass through untouched.
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
    // The host only sees the VST3's opaque state, so we can't redirect takes from out here.
    // Use the plugin's default folder and look for the take this run adds.
    const auto takesFolder = File::getSpecialLocation (File::userMoviesDirectory).getChildFile ("Griddy");
    const auto takesBefore = takesFolder.findChildFiles (File::findFilesAndDirectories, false, "Griddy *");

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

        for (const auto* expected : { "Layout", "Switch every", "Cameras", "Record arm", "Record when" })
            check (names.contains (expected), String ("parameter '") + expected + "' exposed to the host");

        FakePlayHead head;
        plugin->setPlayHead (&head);
        plugin->prepareToPlay (48000.0, 512);

        for (auto* param : plugin->getParameters())
        {
            if (param->getName (64) == "Record arm")
                param->setValueNotifyingHost (1.0f);
            if (param->getName (64) == "Layout")
                param->setValueNotifyingHost (0.5f); // "Cut"
        }

        // Record ~2 seconds while the fake DAW plays and records, then stop the transport.
        runAudio (*plugin, head, 2.0);
        head.playing = head.recording = false;
        runAudio (*plugin, head, 0.5);

        // A take is a .mov plus a .json on macOS, or a folder of JPEGs with take.json elsewhere.
        Array<File> newEntries, takes;
        for (const auto& f : takesFolder.findChildFiles (File::findFilesAndDirectories, false, "Griddy *"))
            if (! takesBefore.contains (f))
                newEntries.add (f);

        for (const auto& f : newEntries)
            if (f.isDirectory() || f.hasFileExtension ("mov"))
                takes.add (f);

        check (takes.size() == 1, "one take recorded while the DAW was recording");

        if (! takes.isEmpty())
        {
            const auto& take = takes.getReference (0);
            const auto json = JSON::parse (take.isDirectory() ? take.getChildFile ("take.json")
                                                              : take.withFileExtension ("json"));
            const auto frames = (int) json["frames"];
            std::cout << "Take: " << take.getFileName() << " (" << frames << " frames)" << std::endl;

            check (frames > 30, "take has more than 1 second of frames");
            check (take.isDirectory() ? take.getNumberOfChildFiles (File::findFiles, "*.jpg") == frames
                                      : take.getSize() > 10000,
                   "video data is on disk");
            check ((int) json["startBar"] == 3, "take knows it started at bar 3");
            check (std::abs ((double) json["bpm"] - 100.0) < 1.0e-9, "take knows the tempo");
        }

        for (const auto& f : newEntries)
            f.deleteRecursively();

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
            if (param->getName (64) == "Layout")
                check (param->getCurrentValueAsText() == "Cut", "layout restored from saved state");
    }

    std::cout << (failures == 0 ? "HOST SMOKE TEST PASSED" : "HOST SMOKE TEST FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
