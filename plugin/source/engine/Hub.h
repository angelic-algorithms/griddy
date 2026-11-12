#pragma once

#include "PlaybackEngine.h"
#include "TransportState.h"
#include "GridExport.h"
#include "VideoEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace griddy
{

/** How the shared grid looks. Changing it changes the picture, never the takes. */
struct GridLayout
{
    Canvas canvas = Canvas::hd720;
    Template shape = Template::grid;
    float gap = 4.0f;                 // pixels between cells at the canvas size
    uint32_t background = 0xff0b0b0fu;
    std::array<int, 9> order { 0, 1, 2, 3, 4, 5, 6, 7, 8 }; // which slot sits in each cell, in reading order
};

/** What one Griddy instance (one plugin on one track) tells the hub.
    The instance's parameters are read through these pointers, so DAW automation and the UI reach the hub without locks. */
struct MemberControls
{
    std::atomic<float>* slot = nullptr;       // 1 to 9, which grid cell this instance owns
    std::atomic<float>* recordArm = nullptr;
    std::atomic<float>* recordMode = nullptr; // 0 DAW records, 1 and 2 DAW plays
    std::atomic<float>* cameras = nullptr;    // smallest grid this instance wants to see
    std::atomic<float>* master = nullptr;     // on: no slot, this instance is the one that arranges the grid
    std::function<void (int)> setSlotParameter; // message thread: show a slot (0 based) in the instance's own parameter
    std::function<void (bool)> setMasterParameter;
    std::function<void (bool)> setArmParameter; // message thread: the hub switches arm off after a one shot take
};

/** One plugin instance as the hub sees it. Created and destroyed by Hub, used by that instance's threads. */
struct Member
{
    explicit Member (MemberControls c) : controls (c) {}

    MemberControls controls;
    std::atomic<int> slot { -1 };

    // Audio thread -> control thread.
    std::atomic<bool> wantRecording { false };
    std::atomic<bool> playing { false };
    std::atomic<bool> hostRecording { false };

    // Control thread -> audio thread. In DAW plays and Armed a take is one shot: once it ends, nothing
    // records until arm goes off and on again, so pressing play again plays the take instead of replacing it.
    std::atomic<bool> takeDone { false };

    // Audio thread -> control thread, for the transport log.
    std::atomic<int64_t> hostSample { 0 };
    std::atomic<double> hostPpq { 0.0 };
    std::atomic<bool> hostLooping { false };

    // Control thread only: what the transport log last wrote for this instance.
    struct Logged { bool valid = false, playing = false, recording = false, looping = false, armed = false; int mode = -1;
                    int64_t sample = 0; double wall = 0.0; bool stuck = false; } logged;
    std::atomic<int64_t> requestSample { 0 };
    std::atomic<double> requestPpq { 0.0 }, requestBpm { 120.0 };
    std::atomic<int> requestNum { 4 }, requestDen { 4 };

    // Control thread only.
    bool recording = false;
    std::atomic<bool> isMaster { false };
    int pendingMasterParam = -1;
    bool masterQueued = false;
    uint64_t lastCameraSerial = 0;
    int pendingParamSlot = -1;
    bool paramQueued = false;
};

/** The one thing every Griddy instance in a DAW process shares.

    A plugin only hears its own track, so each instance owns one slot of one grid: it feeds that slot
    the track's audio (so the slot locks to its own sound wherever the clip is moved) and records
    that slot's takes. The hub owns what must exist once: the camera, the playback engine with its
    nine slots, and the composited grid.

    Slots that no instance owns follow the host position only. The lowest numbered instance (the leader)
    feeds those, because a slot's audio input has exactly one writer at a time.

    Threads:
      audio threads   Hub::processBlock(), one per instance, each touching only its own slot (plus the leader's silence slots)
      control thread  starts and stops takes, passes camera frames to recorders, draws the grid, settles slot choices
      worker          finishes takes (writes files) and rebuilds a slot's sync data
*/
class Hub final : private juce::Thread
{
public:
    static constexpr int maxSlots = VideoEngine::maxSlots;

    Hub();
    ~Hub() override;

    // ---- membership: message thread ----
    Member* addMember (MemberControls controls);
    void removeMember (Member* member);

    /** Message thread, from prepareToPlay. Builds the engine on the first call or when the sample rate changes. */
    void prepare (double sampleRate, int maxBlockSize);

    /** Which project's takes the grid plays. The first instance to say wins. */
    void setSession (const juce::String& sessionId);
    juce::String getSession() const;

    /** Move where takes are saved. Applies at once when nothing is recording or exporting; otherwise it is
        kept as pending and applied by the control thread the moment they end, and this returns false. */
    bool changeOutputFolder (const juce::File& folder);

    VideoEngine& video() noexcept { return videoEngine; }

    /** Audio thread, once per block per instance. */
    void processBlock (Member& member, const juce::AudioBuffer<float>& buffer, const TransportSnapshot& transport) noexcept;

    // ---- message thread ----
    bool keepTake (int slot, int takeId);
    bool retakeTake (int slot, int takeId);
    bool restoreTake (int slot, int takeId);
    bool setSlip (int slot, int takeId, double seconds);

    // ---- grid layout (any instance can change it; the master instance is where the UI puts these controls) ----
    GridLayout getLayout() const;
    void setLayout (Canvas canvas, float gap, uint32_t background, Template shape = Template::grid);
    bool swapCells (int a, int b);
    juce::var layoutToVar() const;
    void applyLayoutVar (const juce::var& v);

    // ---- MP4 export of the grid and the mix ----
    // Arm it, press play in the DAW: the master instance's audio (put it on the master track) and the composited grid are
    // written to <output folder>/Exports/ until the DAW stops. Message thread.
    bool startExport();
    void stopExport();
    enum ExportState { exportIdle = 0, exportWaiting, exportRecording, exportWriting, exportDone, exportFailed };
    int getExportState() const noexcept { return exportState.load(); }

    juce::var getStatus (const Member* member) const;
    juce::MemoryBlock getPreviewJpeg() const;
    int64_t getPreviewSerial() const noexcept { return previewSerial.load(); }

    int numMembers() const;

private:
    void run() override;
    void controlTick();
    void settleSlots();
    void disarmLater (Member* member);
    void logTransport (Member& m);
    void log (const juce::String& line);
    bool applyOutputFolder (const juce::File& folder);
    void buildEngine();
    void destroyEngine();
    void drawPreview();
    void updateExport();
    void finishTake (int slot);
    void reloadLater (int slot);
    juce::var takesToVar (int slot) const;
    int firstFreeSlot() const;   // call with memberLock held

    TransportState unusedTransport;
    VideoEngine videoEngine;

    mutable std::mutex memberLock;
    std::vector<std::unique_ptr<Member>> members;

    mutable std::mutex engineLock;
    std::unique_ptr<PlaybackEngine> engine;
    std::array<std::unique_ptr<TakeRecorder>, maxSlots> recorders;
    std::atomic<PlaybackEngine*> audioEngine { nullptr };
    std::array<std::atomic<TakeRecorder*>, maxSlots> audioRecorders;

    double sampleRate = 0.0;
    int maxBlock = 1024;
    juce::String sessionId;

    std::atomic<Member*> leader { nullptr };
    std::atomic<uint32_t> claimedMask { 0 };
    std::atomic<int> activeSlots { 4 };
    std::atomic<bool> sessionAdopted { false };

    std::array<std::atomic<bool>, maxSlots> slotBusy;
    std::array<std::atomic<bool>, maxSlots> slotRecording;
    std::array<std::atomic<uint64_t>, maxSlots> droppedFrames, droppedAudio;

    mutable std::mutex layoutLock;
    GridLayout layout;

    GridExporter exporter;
    std::atomic<GridExporter*> audioExporter { nullptr };
    std::atomic<int> exportState { exportIdle };
    std::atomic<bool> exportStopRequested { false };
    juce::File exportFile;       // guarded by statusLock
    juce::File pendingFolder;    // guarded by statusLock: a folder chosen while a take or export ran
    juce::String exportError;    // guarded by statusLock

    juce::ThreadPool worker { 1 };

    Compositor compositor;
    juce::Image output { juce::Image::ARGB, 1280, 720, true, juce::SoftwareImageType() };
    juce::Image previewImage { juce::Image::ARGB, VideoEngine::previewWidth, VideoEngine::previewHeight, true, juce::SoftwareImageType() };
    juce::JPEGImageFormat previewFormat;
    mutable std::mutex previewLock;
    juce::MemoryBlock previewJpeg;
    std::atomic<int64_t> previewSerial { 0 };
    int tick = 0;
    std::unique_ptr<juce::FileOutputStream> logFile; // control thread only
    juce::File logFolder;

    mutable std::mutex statusLock;
    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Hub)
};

} // namespace griddy
