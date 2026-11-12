#include "PluginEditor.h"
#include "UiResources.h"

namespace griddy
{

static constexpr auto devServerUrl = "http://localhost:5173/";

static juce::RangedAudioParameter& param (GriddyProcessor& p, const char* id)
{
    auto* rp = p.getParameters().getParameter (id);
    jassert (rp != nullptr);
    return *rp;
}

GriddyEditor::GriddyEditor (GriddyProcessor& p)
    : AudioProcessorEditor (&p),
      processor (p),
      engine (p.getEngine()),
      hub (p.getHub()),
      browser (std::make_unique<juce::WebBrowserComponent> (makeBrowserOptions())),
      slotAttachment (param (p, ParamIDs::slot), slotRelay, nullptr),
      masterAttachment (param (p, ParamIDs::master), masterRelay, nullptr),
      numSlotsAttachment (param (p, ParamIDs::numSlots), numSlotsRelay, nullptr),
      recordArmAttachment (param (p, ParamIDs::recordArm), recordArmRelay, nullptr),
      recordModeAttachment (param (p, ParamIDs::recordMode), recordModeRelay, nullptr)
{
    addAndMakeVisible (*browser);

   #if GRIDDY_UI_DEV_SERVER
    browser->goToURL (devServerUrl);
   #else
    browser->goToURL (juce::WebBrowserComponent::getResourceProviderRoot());
   #endif

    setResizable (true, true);
    setResizeLimits (720, 480, 2400, 1600);
    setSize (1100, 720);

    startTimerHz (30);
}

GriddyEditor::~GriddyEditor()
{
    stopTimer();
}

juce::WebBrowserComponent::Options GriddyEditor::makeBrowserOptions()
{
    using Options = juce::WebBrowserComponent::Options;

    auto options = Options{}
        // WebView2 on Windows (Edge's engine); WKWebView on Mac; WebKitGTK on Linux.
        .withBackend (Options::Backend::webview2)
        .withWinWebView2Options (Options::WinWebView2{}
                                     .withUserDataFolder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                                                              .getChildFile ("GriddyWebView2")))
        .withNativeIntegrationEnabled()
        .withKeepPageLoadedWhenBrowserIsHidden()
        .withOptionsFrom (slotRelay)
        .withOptionsFrom (masterRelay)
        .withOptionsFrom (numSlotsRelay)
        .withOptionsFrom (recordArmRelay)
        .withOptionsFrom (recordModeRelay)
        .withNativeFunction ("getDevices", [this] (const juce::Array<juce::var>&, auto complete)
        {
            complete (devicesToVar (engine.getDevices()));
        })
        .withNativeFunction ("getSlots", [this] (const juce::Array<juce::var>&, auto complete)
        {
            complete (slotsToVar());
        })
        .withNativeFunction ("setSlotSource", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            const auto slot = args.size() > 0 ? (int) args[0] : -1;
            const auto source = args.size() > 1 ? args[1].toString() : juce::String();
            const auto error = engine.setSlotSource (slot, source);

            auto result = std::make_unique<juce::DynamicObject>();
            result->setProperty ("ok", error.isEmpty());
            result->setProperty ("error", error);
            result->setProperty ("slots", slotsToVar());
            complete (juce::var (result.release()));
        })
        .withNativeFunction ("getStatus", [this] (const juce::Array<juce::var>&, auto complete)
        {
            complete (statusToVar());
        })
        .withNativeFunction ("setLayout", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            if (args.size() > 2)
                hub.setLayout ((Canvas) juce::jlimit (0, 3, (int) args[0]), (float) (double) args[1],
                               (uint32_t) args[2].toString().getHexValue32(),
                               (Template) juce::jlimit (0, numTemplates - 1, args.size() > 3 ? (int) args[3] : 0));

            complete (hub.layoutToVar());
        })
        .withNativeFunction ("startExport", [this] (const juce::Array<juce::var>&, auto complete)
        {
            complete (hub.startExport());
        })
        .withNativeFunction ("stopExport", [this] (const juce::Array<juce::var>&, auto complete)
        {
            hub.stopExport();
            complete (true);
        })
        .withNativeFunction ("swapCells", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            complete (args.size() > 1 && hub.swapCells ((int) args[0], (int) args[1]));
        })
        .withNativeFunction ("keepTake", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            complete (args.size() > 1 && hub.keepTake ((int) args[0], (int) args[1]));
        })
        .withNativeFunction ("retakeTake", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            complete (args.size() > 1 && hub.retakeTake ((int) args[0], (int) args[1]));
        })
        .withNativeFunction ("restoreTake", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            complete (args.size() > 1 && hub.restoreTake ((int) args[0], (int) args[1]));
        })
        .withNativeFunction ("setSlip", [this] (const juce::Array<juce::var>& args, auto complete)
        {
            complete (args.size() > 2 && hub.setSlip ((int) args[0], (int) args[1], (double) args[2]));
        })
        .withNativeFunction ("revealOutputFolder", [this] (const juce::Array<juce::var>&, auto complete)
        {
            const auto folder = engine.getOutputFolder();
            folder.createDirectory();
            folder.revealToUser();
            complete (true);
        })
        .withNativeFunction ("chooseOutputFolder", [this] (const juce::Array<juce::var>&, auto complete)
        {
            folderChooser = std::make_unique<juce::FileChooser> ("Where should Griddy save takes?", engine.getOutputFolder());
            folderChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                        [this, complete] (const juce::FileChooser& chooser)
                                        {
                                            // While a take or export runs the hub keeps the choice and moves over when it ends.
                                            if (const auto chosen = chooser.getResult(); chosen != juce::File())
                                                hub.changeOutputFolder (chosen);

                                            complete (engine.getOutputFolder().getFullPathName());
                                        });
        })
        .withResourceProvider ([this] (const auto& url) { return getResource (url); },
                              // Only the dev build lets the dev server's page read our resources.
                             #if GRIDDY_UI_DEV_SERVER
                              juce::URL (devServerUrl).getOrigin()
                             #else
                              std::nullopt
                             #endif
                              );

    return options;
}

std::optional<juce::WebBrowserComponent::Resource> GriddyEditor::getResource (const juce::String& url)
{
    if (url.startsWith ("/preview.jpg"))
    {
        const auto jpeg = hub.getPreviewJpeg();

        if (jpeg.isEmpty())
            return std::nullopt;

        std::vector<std::byte> bytes (jpeg.getSize());
        std::memcpy (bytes.data(), jpeg.getData(), jpeg.getSize());
        return juce::WebBrowserComponent::Resource { std::move (bytes), "image/jpeg" };
    }

    return getEmbeddedUiResource (url);
}

void GriddyEditor::timerCallback()
{
    browser->emitEventIfBrowserIsVisible ("transport", transportToVar());

    if (const auto serial = hub.getPreviewSerial(); serial != lastPreviewSerial)
    {
        lastPreviewSerial = serial;
        browser->emitEventIfBrowserIsVisible ("preview", (juce::int64) serial);
    }

    if ((ticks++ % 15) == 0)
        browser->emitEventIfBrowserIsVisible ("status", statusToVar());
}

juce::var GriddyEditor::devicesToVar (const std::vector<CaptureDeviceInfo>& devices)
{
    juce::Array<juce::var> list;

    for (const auto& d : devices)
    {
        auto obj = std::make_unique<juce::DynamicObject>();
        obj->setProperty ("id", d.id);
        obj->setProperty ("name", d.name);
        obj->setProperty ("kind", d.kind);
        list.add (juce::var (obj.release()));
    }

    return list;
}

juce::var GriddyEditor::statusToVar() const
{
    auto status = engine.getStatus();

    if (auto* obj = status.getDynamicObject())
        obj->setProperty ("engine", hub.getStatus (processor.getMember()));

    return status;
}

juce::var GriddyEditor::slotsToVar() const
{
    juce::Array<juce::var> list;

    for (const auto& id : engine.getSlotSources())
        list.add (id);

    return list;
}

juce::var GriddyEditor::transportToVar() const
{
    const auto t = processor.getTransport();
    const auto barLen = (double) t.timeSigNumerator * 4.0 / (double) juce::jmax (1, t.timeSigDenominator);
    const auto beatLen = 4.0 / (double) juce::jmax (1, t.timeSigDenominator);

    // Bars and beats the way a DAW shows them: both start at 1.
    const auto bar = (int) std::floor (t.ppqPosition / barLen) + 1;
    const auto beat = (int) std::floor (std::fmod (juce::jmax (0.0, t.ppqPosition), barLen) / beatLen) + 1;

    auto obj = std::make_unique<juce::DynamicObject>();
    obj->setProperty ("bpm", t.bpm);
    obj->setProperty ("ppq", t.ppqPosition);
    obj->setProperty ("bar", bar);
    obj->setProperty ("beat", beat);
    obj->setProperty ("timeSig", juce::String (t.timeSigNumerator) + "/" + juce::String (t.timeSigDenominator));
    obj->setProperty ("playing", t.isPlaying);
    obj->setProperty ("recording", t.isRecording);
    obj->setProperty ("hostSync", t.hostProvidedPosition);
    return juce::var (obj.release());
}

void GriddyEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff0b0b0f));
}

void GriddyEditor::resized()
{
    browser->setBounds (getLocalBounds());
}

} // namespace griddy
