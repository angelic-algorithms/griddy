#include "TakeFormat.h"

#include <cmath>

namespace griddy
{

const char* TakeMeta::stateName (State s)
{
    switch (s)
    {
        case State::recording: return "recording";
        case State::review:    return "review";
        case State::kept:      return "kept";
        case State::archived:  return "archived";
        case State::discarded: return "discarded";
    }
    return "recording";
}

TakeMeta::State TakeMeta::stateFromName (const juce::String& n)
{
    for (auto s : { State::recording, State::review, State::kept, State::archived, State::discarded })
        if (n == stateName (s))
            return s;
    return State::recording;
}

juce::var TakeMeta::toVar() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("takeId", (int) takeId);
    o->setProperty ("slot", slot);
    o->setProperty ("state", stateName (state));
    o->setProperty ("sampleRate", sampleRate);
    o->setProperty ("startHostSample", (juce::int64) startHostSample);
    o->setProperty ("startPpq", startPpq);
    o->setProperty ("startBar", (int) std::floor (startPpq / (timeSigNumerator * 4.0 / timeSigDenominator)) + 1);
    o->setProperty ("bpm", bpm);
    o->setProperty ("timeSigNumerator", timeSigNumerator);
    o->setProperty ("timeSigDenominator", timeSigDenominator);
    o->setProperty ("cameraLatencySeconds", cameraLatencySeconds);
    o->setProperty ("deliveryDelaySeconds", deliveryDelaySeconds);
    o->setProperty ("audioLatencySeconds", audioLatencySeconds);
    o->setProperty ("slipSeconds", slipSeconds);
    o->setProperty ("durationSeconds", durationSeconds);
    o->setProperty ("width", width);
    o->setProperty ("height", height);
    o->setProperty ("proxyWidth", proxyWidth);
    o->setProperty ("proxyHeight", proxyHeight);
    o->setProperty ("fps", fps);
    o->setProperty ("frames", numFrames);
    o->setProperty ("embeddedSync", embeddedSync);
    o->setProperty ("embeddedTag", embeddedTag);
    o->setProperty ("createdAt", createdAt);

    juce::Array<juce::var> times, env;
    for (auto t : frameTimes)
        times.add (t);
    for (auto e : envelope)
        env.add ((double) e);
    o->setProperty ("frameTimes", times);
    o->setProperty ("envelope", env);
    return juce::var (o);
}

TakeMeta TakeMeta::fromVar (const juce::var& v)
{
    TakeMeta m;
    m.takeId = (int) v.getProperty ("takeId", -1);
    m.slot = (int) v.getProperty ("slot", 0);
    m.state = stateFromName (v.getProperty ("state", "recording").toString());
    m.sampleRate = (double) v.getProperty ("sampleRate", 48000.0);
    m.startHostSample = (juce::int64) v.getProperty ("startHostSample", 0);
    m.startPpq = (double) v.getProperty ("startPpq", 0.0);
    m.bpm = (double) v.getProperty ("bpm", 120.0);
    m.timeSigNumerator = (int) v.getProperty ("timeSigNumerator", 4);
    m.timeSigDenominator = (int) v.getProperty ("timeSigDenominator", 4);
    m.cameraLatencySeconds = (double) v.getProperty ("cameraLatencySeconds", 0.0);
    m.deliveryDelaySeconds = (double) v.getProperty ("deliveryDelaySeconds", 0.0);
    m.audioLatencySeconds = (double) v.getProperty ("audioLatencySeconds", 0.0);
    m.slipSeconds = (double) v.getProperty ("slipSeconds", 0.0);
    m.durationSeconds = (double) v.getProperty ("durationSeconds", 0.0);
    m.width = (int) v.getProperty ("width", 1280);
    m.height = (int) v.getProperty ("height", 720);
    m.proxyWidth = (int) v.getProperty ("proxyWidth", 640);
    m.proxyHeight = (int) v.getProperty ("proxyHeight", 360);
    m.fps = (double) v.getProperty ("fps", 30.0);
    m.numFrames = (int) v.getProperty ("frames", 0);
    m.embeddedSync = (bool) v.getProperty ("embeddedSync", false);
    m.embeddedTag = (int) v.getProperty ("embeddedTag", 0);
    m.createdAt = (double) v.getProperty ("createdAt", 0.0);

    if (auto* t = v.getProperty ("frameTimes", {}).getArray())
        for (const auto& x : *t)
            m.frameTimes.push_back ((double) x);

    if (auto* e = v.getProperty ("envelope", {}).getArray())
        for (const auto& x : *e)
            m.envelope.push_back ((float) (double) x);

    return m;
}

bool writeTakeMeta (const juce::File& folder, const TakeMeta& meta)
{
    // Write to a temporary name and rename, so a crash never leaves half a file.
    const auto tmp = folder.getChildFile ("take.json.tmp");
    const auto target = folder.getChildFile ("take.json");

    if (! tmp.replaceWithText (juce::JSON::toString (meta.toVar(), false)))
        return false;

    return tmp.moveFileTo (target);
}

bool readTakeMeta (const juce::File& folder, TakeMeta& meta)
{
    const auto f = folder.getChildFile ("take.json");
    if (! f.existsAsFile())
        return false;

    const auto v = juce::JSON::parse (f);
    if (! v.isObject())
        return false;

    meta = TakeMeta::fromVar (v);
    return true;
}

//==============================================================================
bool WavWriter::open (const juce::File& file, double sampleRate)
{
    file.deleteFile();
    out = std::make_unique<juce::FileOutputStream> (file);
    if (out->failedToOpen())
    {
        out.reset();
        return false;
    }

    sr = sampleRate;
    count = 0;
    writeHeader();
    return true;
}

void WavWriter::writeHeader()
{
    const auto dataBytes = (uint32_t) (count * 2);
    out->setPosition (0);
    out->write ("RIFF", 4);
    out->writeInt (36 + (int) dataBytes);
    out->write ("WAVEfmt ", 8);
    out->writeInt (16);
    out->writeShort (1);          // PCM
    out->writeShort (1);          // mono
    out->writeInt ((int) sr);
    out->writeInt ((int) sr * 2);
    out->writeShort (2);
    out->writeShort (16);
    out->write ("data", 4);
    out->writeInt ((int) dataBytes);
}

void WavWriter::write (const float* samples, int n)
{
    if (out == nullptr)
        return;

    out->setPosition (44 + count * 2);

    std::vector<int16_t> tmp ((size_t) n);
    for (int i = 0; i < n; ++i)
        tmp[(size_t) i] = (int16_t) juce::jlimit (-32767.0f, 32767.0f, samples[i] * 32767.0f);

    out->write (tmp.data(), (size_t) n * 2);
    count += n;

    // Keep the header valid every so often so a crash leaves a readable file.
    if ((count / n) % 64 == 0)
    {
        writeHeader();
        out->setPosition (44 + count * 2);
    }
}

void WavWriter::close()
{
    if (out == nullptr)
        return;

    writeHeader();
    out->flush();
    out.reset();
}

bool readWav (const juce::File& file, std::vector<float>& samples, double& sampleRate)
{
    juce::FileInputStream in (file);
    if (in.failedToOpen() || in.getTotalLength() < 44)
        return false;

    char tag[4];
    in.read (tag, 4);
    in.readInt();
    in.read (tag, 4);
    in.read (tag, 4);
    in.readInt();
    in.readShort();
    in.readShort();
    sampleRate = (double) in.readInt();
    in.readInt();
    in.readShort();
    in.readShort();
    in.read (tag, 4);
    in.readInt();

    // The size in the header may be stale after a crash, so trust the file length.
    const auto n = (size_t) ((in.getTotalLength() - 44) / 2);
    std::vector<int16_t> raw (n);
    in.read (raw.data(), (int) (n * 2));

    samples.resize (n);
    for (size_t i = 0; i < n; ++i)
        samples[i] = (float) raw[i] / 32767.0f;

    return true;
}

std::vector<float> computeEnvelope (const float* audio, size_t n, double sampleRate, double step)
{
    std::vector<float> env;
    const auto w = (size_t) (sampleRate * step);

    for (size_t i = 0; i + w <= n; i += w)
    {
        double s = 0.0;
        for (size_t j = 0; j < w; ++j)
            s += (double) audio[i + j] * audio[i + j];
        env.push_back ((float) std::sqrt (s / (double) w));
    }

    return env;
}

} // namespace griddy
