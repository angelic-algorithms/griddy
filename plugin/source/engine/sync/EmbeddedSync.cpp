#include "EmbeddedSync.h"

#include <algorithm>
#include <cmath>

namespace griddy::embedded
{

static constexpr double twoPi = 6.283185307179586476925286766559;

uint8_t crc8 (uint8_t tag, uint16_t counter)
{
    uint8_t bytes[3] = { tag, (uint8_t) (counter >> 8), (uint8_t) (counter & 0xff) };
    uint8_t crc = 0;

    for (auto b : bytes)
    {
        crc ^= b;
        for (int i = 0; i < 8; ++i)
            crc = (uint8_t) ((crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1));
    }

    return crc;
}

//==============================================================================
Encoder::Encoder (double sampleRate, uint8_t takeTag, float amplitude)
    : sr (sampleRate), symLen (symbolSamples (sampleRate)), tag (takeTag), amp (amplitude)
{
}

bool Encoder::bitForSymbol (int64_t symbol) const
{
    // Symbol 0 is the phase reference and carries no bit.
    const auto bitIndex = symbol - 1;
    const auto frame = bitIndex / frameBits;
    const auto j = (int) (bitIndex % frameBits);

    const auto counter = (uint16_t) (frame & 0xffff);
    uint64_t word = ((uint64_t) syncWord << 32) | ((uint64_t) tag << 24) | ((uint64_t) counter << 8) | crc8 (tag, counter);
    return ((word >> (frameBits - 1 - j)) & 1u) != 0;
}

void Encoder::mixInto (float* audio, int numSamples)
{
    for (int i = 0; i < numSamples; ++i, ++sampleIndex)
    {
        const auto symbol = sampleIndex / symLen;

        while (currentSymbol < symbol)
        {
            ++currentSymbol;

            if (currentSymbol > 0 && bitForSymbol (currentSymbol))
                symbolPhase += twoPi / 2.0;
        }

        const auto phase = twoPi * carrierHz * (double) sampleIndex / sr + symbolPhase;
        audio[i] += amp * (float) std::cos (phase);
    }
}

//==============================================================================
Decoder::Decoder (double sampleRate) : symLen (symbolSamples (sampleRate)), sr (sampleRate)
{
    histSize = (size_t) ((frameBits + 4) * symLen);
    reset();
}

static void makeHighPass (double sr, double freq, double q, double& b0, double& b1, double& b2, double& a1, double& a2)
{
    const auto w0 = twoPi * freq / sr;
    const auto cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
    const auto a0 = 1.0 + alpha;
    b0 = (1.0 + cw) / 2.0 / a0;
    b1 = -(1.0 + cw) / a0;
    b2 = b0;
    a1 = -2.0 * cw / a0;
    a2 = (1.0 - alpha) / a0;
}

void Decoder::reset()
{
    // Strong low frequency music leaks through the demodulator's boxcar filter, so the
    // band around the carrier is isolated first. Butterworth 4th order at 16 kHz.
    makeHighPass (sr, 16000.0, 0.5412, hp[0].b0, hp[0].b1, hp[0].b2, hp[0].a1, hp[0].a2);
    makeHighPass (sr, 16000.0, 1.3066, hp[1].b0, hp[1].b1, hp[1].b2, hp[1].a1, hp[1].a2);
    hp[0].z1 = hp[0].z2 = hp[1].z1 = hp[1].z2 = 0.0;
    n = 0;
    oscRe = 1.0;
    oscIm = 0.0;
    rotRe = std::cos (twoPi * carrierHz / sr);
    rotIm = -std::sin (twoPi * carrierHz / sr);
    zHist.assign ((size_t) symLen, C {});
    yHist.assign (histSize, C {});
    dHist.assign (histSize, 0.0f);
    magHist.assign (histSize, 0.0f);
    sum = {};
    haveCandidate = false;
    bestScore = 0.0f;
    lastReportedEnd = -1;
}

bool Decoder::testAt (int64_t index, Detection& d, float& score) const
{
    // Symbol windows end at index - m * symLen. A frame needs the 48 bit decisions plus the
    // window before the first one as the differential reference.
    const auto L = (int64_t) symLen;

    if (index + 1 < (int64_t) (frameBits + 1) * L)
        return false;

    uint64_t word = 0;
    for (int j = 0; j < frameBits; ++j)
    {
        const auto idx = index - (int64_t) (frameBits - 1 - j) * L;
        word = (word << 1) | (dAt (idx) < 0.0f ? 1u : 0u);
    }

    if ((uint16_t) (word >> 32) != syncWord)
        return false;

    const auto tag = (uint8_t) ((word >> 24) & 0xff);
    const auto counter = (uint16_t) ((word >> 8) & 0xffff);

    if ((uint8_t) (word & 0xff) != crc8 (tag, counter))
        return false;

    // The carrier must really be there in every window, at a steady level. This is what keeps
    // noise and silence from ever producing a false frame.
    float total = 0.0f, smallest = 1.0e9f;
    for (int m = 0; m <= frameBits; ++m)
    {
        const auto mag = magAt (index - (int64_t) m * L);
        total += mag;
        smallest = std::min (smallest, mag);
    }

    const auto mean = total / (float) (frameBits + 1);
    const auto nominal = defaultAmplitude * 0.5f * (float) symLen; // |y| for the default amplitude
    const auto floorMag = 0.15f * nominal;

    if (smallest < floorMag || smallest < 0.5f * mean)
        return false;

    d.endIndex = index + 1;
    d.takeTag = tag;
    d.counter = counter;
    d.takeSample = (int64_t) (frameBits * (int64_t) counter + frameBits + 1) * L;
    d.quality = mean / nominal;
    score = total;
    return true;
}

void Decoder::push (const float* audio, int numSamples, std::vector<Detection>& out)
{
    const auto L = (int64_t) symLen;

    for (int i = 0; i < numSamples; ++i, ++n)
    {
        const double x = hp[1].process (hp[0].process ((double) audio[i]));
        C z { x * oscRe, x * oscIm };

        const auto slot = (size_t) (n % L);
        sum.re += z.re - zHist[slot].re;
        sum.im += z.im - zHist[slot].im;
        zHist[slot] = z;

        // advance the oscillator
        const auto nr = oscRe * rotRe - oscIm * rotIm;
        const auto ni = oscRe * rotIm + oscIm * rotRe;
        oscRe = nr;
        oscIm = ni;

        if ((n & 4095) == 4095)
        {
            const auto norm = 1.0 / std::sqrt (oscRe * oscRe + oscIm * oscIm);
            oscRe *= norm;
            oscIm *= norm;
        }

        const auto hi = (size_t) (n % (int64_t) histSize);
        const auto prev = n >= L ? yHist[(size_t) ((n - L) % (int64_t) histSize)] : C {};
        yHist[hi] = sum;
        dHist[hi] = (float) (sum.re * prev.re + sum.im * prev.im);
        magHist[hi] = (float) std::sqrt (sum.re * sum.re + sum.im * sum.im);

        // dHist holds the raw product; its sign is the differential bit.
        Detection d;
        float score = 0.0f;

        if (testAt (n, d, score) && d.endIndex > lastReportedEnd + 8 * L)
        {
            if (! haveCandidate)
            {
                haveCandidate = true;
                candidateFirst = n;
                best = d;
                bestScore = score;
            }
            else if (score > bestScore)
            {
                best = d;
                bestScore = score;
            }
        }

        // Let the alignment settle for one symbol length after the first pass, then keep the best.
        if (haveCandidate && n >= candidateFirst + L)
        {
            out.push_back (best);
            lastReportedEnd = best.endIndex;
            haveCandidate = false;
            bestScore = 0.0f;
        }
    }
}

//==============================================================================
EmbeddedSyncClipSource::EmbeddedSyncClipSource (double sampleRate)
    : sr (sampleRate),
      audio ((size_t) (sampleRate * 2.0)),
      stamps (4096),
      decoder (sampleRate)
{
    scratch.resize (8192);
}

void EmbeddedSyncClipSource::feed (const float* mono, int numSamples, int64_t hostSample, bool playing) noexcept
{
    // Only analyse while the transport runs, so a stopped DAW costs nothing.
    if (! playing || numSamples <= 0)
        return;

    const auto stored = audio.write (mono, (size_t) numSamples);
    Stamp s { streamWritten, hostSample, (int32_t) stored, 0 };
    stamps.write (&s, 1);
    streamWritten += (int64_t) stored;
}

void EmbeddedSyncClipSource::process()
{
    Stamp s;
    while (stamps.read (&s, 1) == 1)
        recent.push_back (s);

    for (;;)
    {
        const auto got = audio.read (scratch.data(), scratch.size());
        if (got == 0)
            break;

        found.clear();
        decoder.push (scratch.data(), (int) got, found);

        for (const auto& d : found)
        {
            // Find the block that contained the sample just before the frame end.
            const auto last = d.endIndex - 1;
            for (auto it = recent.rbegin(); it != recent.rend(); ++it)
            {
                if (it->streamStart <= last)
                {
                    const auto hostAtEnd = it->hostSample + (d.endIndex - it->streamStart);
                    ClipFix f;
                    f.takeId = d.takeTag;
                    f.takeSeconds = (double) d.takeSample / sr;
                    f.hostSeconds = (double) hostAtEnd / sr;
                    f.confidence = std::min (1.0f, d.quality);
                    f.kind = SyncKind::embedded;
                    f.locked = true;
                    f.serial = ++serial;
                    fix.write (f);
                    ++frames;
                    break;
                }
            }
        }
    }

    // Keep only stamps that can still matter (the last few seconds of blocks).
    const auto keepFrom = decoder.streamPosition() - (int64_t) (sr * 3.0);
    while (recent.size() > 2 && recent[1].streamStart < keepFrom)
        recent.pop_front();
}

} // namespace griddy::embedded
