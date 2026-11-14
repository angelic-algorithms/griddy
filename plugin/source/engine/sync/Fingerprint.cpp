#include "Fingerprint.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>

namespace griddy::fingerprint
{

static constexpr double pi = 3.14159265358979323846;

static void fft (double* re, double* im, int n)
{
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;

        if (i < j)
        {
            std::swap (re[i], re[j]);
            std::swap (im[i], im[j]);
        }
    }

    for (int len = 2; len <= n; len <<= 1)
    {
        const auto ang = -2.0 * pi / len;
        const auto wr = std::cos (ang), wi = std::sin (ang);

        for (int i = 0; i < n; i += len)
        {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < len / 2; ++k)
            {
                const auto ur = re[i + k], ui = im[i + k];
                const auto vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                const auto vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr;
                im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr;
                im[i + k + len / 2] = ui - vi;
                const auto nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

//==============================================================================
static void makeLowPass (double sr, double freq, double q, double& b0, double& b1, double& b2, double& a1, double& a2)
{
    const auto w0 = 2.0 * pi * freq / sr;
    const auto cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
    const auto a0 = 1.0 + alpha;
    b0 = (1.0 - cw) / 2.0 / a0;
    b1 = (1.0 - cw) / a0;
    b2 = b0;
    a1 = -2.0 * cw / a0;
    a2 = (1.0 - alpha) / a0;
}

Analyzer::Analyzer (double inputRate, bool keepAllAudio)
    : ratio (inputRate / analysisRate), keepAll (keepAllAudio)
{
    const auto cutoff = std::min (3600.0, inputRate * 0.45);
    makeLowPass (inputRate, cutoff, 0.5412, lp[0].b0, lp[0].b1, lp[0].b2, lp[0].a1, lp[0].a2);
    makeLowPass (inputRate, cutoff, 1.3066, lp[1].b0, lp[1].b1, lp[1].b2, lp[1].a1, lp[1].a2);

    window.resize (fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * pi * i / fftSize));

    ring.assign (fftSize, 0.0f);
    re.resize (fftSize);
    im.resize (fftSize);
    mag.resize (fftSize / 2);
}

void Analyzer::push (const float* audio, int numSamples, std::vector<Landmark>& out)
{
    for (int i = 0; i < numSamples; ++i, ++inIndex)
    {
        const auto y = lp[1].process (lp[0].process ((double) audio[i]));

        while (nextOut <= (double) inIndex)
        {
            const auto frac = nextOut - (double) (inIndex - 1);
            const auto v = (float) (prevY + (y - prevY) * std::clamp (frac, 0.0, 1.0));
            nextOut += ratio;

            ring[(size_t) (produced8k % fftSize)] = v;
            history8k.push_back (v);
            ++produced8k;

            if (produced8k >= fftSize && ((produced8k - fftSize) % hop) == 0)
                processFrame (out);
        }

        prevY = y;
    }

    if (! keepAll && history8k.size() > 4 * analysisRate)
        history8k.erase (history8k.begin(), history8k.end() - 2 * analysisRate);
}

void Analyzer::processFrame (std::vector<Landmark>& out)
{
    const auto frame = (int32_t) ((produced8k - fftSize) / hop);

    for (int i = 0; i < fftSize; ++i)
    {
        re[(size_t) i] = (double) ring[(size_t) ((produced8k - fftSize + i) % fftSize)] * (double) window[(size_t) i];
        im[(size_t) i] = 0.0;
    }

    fft (re.data(), im.data(), fftSize);

    float frameMax = 0.0f;
    for (int k = 4; k < 250; ++k)
    {
        mag[(size_t) k] = (float) std::sqrt (re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k]);
        frameMax = std::max (frameMax, mag[(size_t) k]);
    }

    ++framesDone;
    std::vector<Peak> peaks;
    const auto threshold = std::max (0.15f, 0.12f * frameMax);

    for (int k = 8; k < 246; ++k)
    {
        const auto m = mag[(size_t) k];
        if (m < threshold)
            continue;

        bool isMax = true;
        for (int d = -4; d <= 4 && isMax; ++d)
            if (d != 0 && mag[(size_t) (k + d)] >= m)
                isMax = false;

        if (isMax)
            peaks.push_back ({ frame, (int16_t) k });
    }

    std::sort (peaks.begin(), peaks.end(), [&] (const Peak& a, const Peak& b) { return mag[(size_t) a.bin] > mag[(size_t) b.bin]; });
    if (peaks.size() > 4)
        peaks.resize (4);

    while (! recentPeaks.empty() && recentPeaks.front().frame < frame - 63)
        recentPeaks.pop_front();

    for (const auto& p2 : peaks)
    {
        int fan = 0;
        for (auto it = recentPeaks.rbegin(); it != recentPeaks.rend() && fan < 5; ++it)
        {
            const auto dt = p2.frame - it->frame;
            const auto df = (int) p2.bin - (int) it->bin;

            if (dt < 2 || dt > 63 || std::abs (df) > 63)
                continue;

            const auto h = (uint32_t) (((it->bin & 0xff) << 13) | ((df + 64) << 6) | dt);
            out.push_back ({ h, p2.frame });
            ++fan;
        }
    }

    for (const auto& p : peaks)
        recentPeaks.push_back (p);
}

//==============================================================================
Index::Index (const float* audio, size_t numSamples, double sampleRate)
{
    durationSeconds = (double) numSamples / sampleRate;

    Analyzer a (sampleRate, true);
    std::vector<Landmark> lm;
    const size_t chunk = 16384;

    for (size_t i = 0; i < numSamples; i += chunk)
        a.push (audio + i, (int) std::min (chunk, numSamples - i), lm);

    entries.reserve (lm.size());
    for (const auto& l : lm)
        entries.push_back (((uint64_t) l.hash << 32) | (uint32_t) l.frame);

    std::sort (entries.begin(), entries.end());

    const auto& h = a.recent8k();
    audio8k.resize (h.size());
    for (size_t i = 0; i < h.size(); ++i)
        audio8k[i] = (int16_t) std::clamp (h[i] * 32000.0f, -32767.0f, 32767.0f);
}

std::pair<std::vector<uint64_t>::const_iterator, std::vector<uint64_t>::const_iterator> Index::find (uint32_t hash) const
{
    const uint64_t lo = (uint64_t) hash << 32, hi = ((uint64_t) hash << 32) | 0xffffffffull;
    return { std::lower_bound (entries.begin(), entries.end(), lo), std::upper_bound (entries.begin(), entries.end(), hi) };
}

//==============================================================================
static constexpr int voteWindowFrames = 250; // 2 seconds

void Matcher::setIndexes (std::vector<std::shared_ptr<const Index>> idx)
{
    indexes = std::move (idx);
    votes.clear();
    counts.clear();
}

void Matcher::addLandmarks (const std::vector<Landmark>& lm)
{
    for (const auto& l : lm)
    {
        for (int i = 0; i < (int) indexes.size(); ++i)
        {
            const auto [b, e] = indexes[(size_t) i]->find (l.hash);

            if (e - b > 24) // a hash this common says nothing
                continue;

            for (auto it = b; it != e; ++it)
            {
                const auto refFrame = (int32_t) (*it & 0xffffffffull);
                const auto offset = refFrame - l.frame;
                votes.push_back ({ l.frame, i, offset });
                ++counts[key (i, offset)];
            }
        }
    }
}

void Matcher::expire (int32_t liveFrame)
{
    while (! votes.empty() && votes.front().liveFrame < liveFrame - voteWindowFrames)
    {
        const auto& v = votes.front();
        const auto k = key (v.takeIndex, v.offset);
        if (--counts[k] <= 0)
            counts.erase (k);
        votes.pop_front();
    }
}

Match Matcher::best (int32_t) const
{
    Match m;
    int bestScore = 0, secondScore = 0;
    int bestTake = -1;
    int32_t bestOff = 0;

    auto count = [&] (int take, int32_t off)
    {
        const auto it = counts.find (key (take, off));
        return it == counts.end() ? 0 : it->second;
    };

    for (const auto& [k, c] : counts)
    {
        const auto take = (int) (k >> 32);
        const auto off = (int32_t) (uint32_t) (k & 0xffffffffull);
        const auto score = count (take, off - 1) + c + count (take, off + 1);

        if (score > bestScore)
        {
            if (take != bestTake || std::abs (off - bestOff) > 2)
                secondScore = bestScore;

            bestScore = score;
            bestTake = take;
            bestOff = off;
        }
        else if (score > secondScore && (take != bestTake || std::abs (off - bestOff) > 2))
        {
            secondScore = score;
        }
    }

    if (bestTake < 0 || bestScore < 6 || bestScore * 4 < secondScore * 5)
        return m;

    m.takeIndex = bestTake;
    m.votes = bestScore;
    m.confidence = std::min (1.0f, (float) bestScore / 30.0f);
    const_cast<Matcher*> (this)->bestOffset = bestOff;
    return m;
}

//==============================================================================
FingerprintClipSource::FingerprintClipSource (double sampleRate)
    : sr (sampleRate),
      audio ((size_t) (sampleRate * 4.0)),
      stamps (4096),
      breaks (256),
      analyzer (sampleRate)
{
    scratch.resize (4096);
}

void FingerprintClipSource::setIndexes (std::vector<std::shared_ptr<const Index>> idx)
{
    const std::scoped_lock sl (indexLock);
    pendingIndexes = std::move (idx);
    haveNewIndexes = true;
}

void FingerprintClipSource::setCandidates (std::vector<AlignedCandidate> c)
{
    const std::scoped_lock sl (indexLock);
    pendingCandidates = std::move (c);
    haveNewCandidates = true;
}

void FingerprintClipSource::feed (const float* mono, int numSamples, int64_t hostSample, bool playing) noexcept
{
    if (! playing || numSamples <= 0)
        return;

    if (expectedHostSample >= 0 && std::abs (hostSample - expectedHostSample) > 32)
    {
        const auto at = streamWritten;
        breaks.write (&at, 1);
    }

    expectedHostSample = hostSample + numSamples;
    const auto stored = audio.write (mono, (size_t) numSamples);
    Stamp s { streamWritten, hostSample, (int32_t) stored, 0 };
    stamps.write (&s, 1);
    streamWritten += (int64_t) stored;
}

void FingerprintClipSource::process()
{
    if (haveNewIndexes.exchange (false))
    {
        const std::scoped_lock sl (indexLock);
        matcher.setIndexes (pendingIndexes);
    }

    if (haveNewCandidates.exchange (false))
    {
        {
            const std::scoped_lock sl (indexLock);
            candidates = pendingCandidates;
        }

        winner = challenger = -1;
        challengerWins = poorWindows = silentWindows = 0;
        wasGood = false;
        auto f = alignedFix.read();
        f.locked = false;
        f.serial = ++alignedSerial;
        alignedFix.write (f);
    }

    Stamp st;
    while (stamps.read (&st, 1) == 1)
        recent.push_back (st);

    int64_t brk;
    while (breaks.read (&brk, 1) == 1)
        pendingBreaks.push_back (brk);

    for (;;)
    {
        auto want = scratch.size();
        if (! pendingBreaks.empty())
        {
            if (pendingBreaks.front() <= streamConsumed)
            {
                pendingBreaks.pop_front();
                matcher.clearVotes();
                analyzer.breakContinuity();
                activeSamples = 0;
                contiguous8k = 0;
                continue;
            }

            want = std::min<size_t> (want, (size_t) (pendingBreaks.front() - streamConsumed));
        }

        const auto got = audio.read (scratch.data(), want);
        if (got == 0)
            break;

        double sumSq = 0.0;
        for (size_t i = 0; i < got; ++i)
            sumSq += (double) scratch[i] * scratch[i];

        if (std::sqrt (sumSq / (double) got) > 0.002)
            activeSamples += (int64_t) got;
        else
            activeSamples = 0;

        landmarks.clear();
        const auto framesBefore = analyzer.frameCount();
        const auto producedBefore = analyzer.samples8kProduced();
        analyzer.push (scratch.data(), (int) got, landmarks);
        streamConsumed += (int64_t) got;
        contiguous8k += analyzer.samples8kProduced() - producedBefore;

        // Aligned comp detection: every 25 ms of audio (50 ms while a slot records), and every 6 ms while waiting
        // for the first sound after a silence, so a gap ends on the right take.
        const auto alignedStep = winner < 0 && silentWindows > 0 ? 50 : (light.load (std::memory_order_relaxed) ? 400 : 200);
        if (alignedOn.load (std::memory_order_relaxed) && analyzer.samples8kProduced() - lastAlignedAt >= alignedStep)
            evaluateAligned();

        matcher.addLandmarks (landmarks);
        matcher.expire ((int32_t) analyzer.frameCount());
        framesSinceEval += analyzer.frameCount() - framesBefore;
        framesAnalysed = analyzer.frameCount();

        if (framesSinceEval >= 16)
        {
            framesSinceEval = 0;
            evaluate();
        }
    }

    const auto keepFrom = streamConsumed - (int64_t) (sr * 5.0);
    while (recent.size() > 2 && recent[1].streamStart < keepFrom)
        recent.pop_front();
}

void FingerprintClipSource::evaluate()
{
    const auto m = matcher.best ((int32_t) analyzer.frameCount());

    if (m.takeIndex < 0)
    {
        unmatched = activeSamples > (int64_t) (sr * 2.0);

        if (unmatched.load())
        {
            auto f = fix.read();
            if (f.locked)
            {
                f.locked = false;
                f.serial = ++serial;
                fix.write (f);
            }
        }

        return;
    }

    unmatched = false;

    // Coarse position from the vote offset, then refine by correlating the last half second
    // of 8 kHz audio with the reference around that position.
    const auto& idx = matcher.index (m.takeIndex);
    const auto produced = analyzer.samples8kProduced();
    const auto coarseEnd = produced + (int64_t) matcher.bestOffset * hop; // reference sample matching the newest live sample

    const auto& live = analyzer.recent8k();
    const int window = std::min<int> (4000, (int) live.size());
    float bestCorr = -1.0f;
    int bestLag = 0;

    for (int lag = -96; lag <= 96; ++lag)
    {
        const auto refStart = coarseEnd - window + lag;
        if (refStart < 0 || refStart + window > (int64_t) idx.audio8k.size())
            continue;

        double num = 0.0, ea = 0.0, eb = 0.0;
        for (int i = 0; i < window; ++i)
        {
            const double a = live[live.size() - (size_t) window + (size_t) i];
            const double b = (double) idx.audio8k[(size_t) (refStart + i)] / 32000.0;
            num += a * b;
            ea += a * a;
            eb += b * b;
        }

        const auto c = (float) (num / std::sqrt (ea * eb + 1.0e-12));
        if (c > bestCorr)
        {
            bestCorr = c;
            bestLag = lag;
        }
    }

    if (bestCorr < 0.5f)
        return; // the votes were wrong or the audio was altered; wait for better evidence

    const auto refEnd = coarseEnd + bestLag;

    // How far this take really sits from where it was recorded: centres the aligned detector's search on the latency
    // the host adds. Only for a fix close to a recorded position; a moved clip says nothing about latency.
    if (const auto host = hostSecondsOfNewest(); host >= 0.0)
    {
        const auto takeSeconds = (double) refEnd / analysisRate;
        for (const auto& c : candidates)
        {
            if (c.takeId != idx.takeId || host < c.hostStart || host > c.hostEnd)
                continue;

            // In a comp the votes mix every take, so a single fix can name the wrong take at a plausible offset. Only a
            // confident fix that repeats, for the take comp detection has chosen (or before it has chosen), moves it.
            const auto d = takeSeconds - (c.takeAtHostStart + (host - c.hostStart));
            const auto repeats = std::abs (d - lastFixDelta) < 0.002;
            lastFixDelta = d;

            if (std::abs (d) < 0.05 && repeats && bestCorr >= 0.7f && (winner < 0 || candidates[(size_t) winner].takeId == idx.takeId))
            {
                if (std::abs (d - latencyDelta) > 0.001)
                    lag = 0;
                latencyDelta = d;
            }
            break;
        }
    }

    for (auto it = recent.rbegin(); it != recent.rend(); ++it)
    {
        if (it->streamStart <= streamConsumed - 1)
        {
            ClipFix f;
            f.takeId = idx.takeId;
            f.takeSeconds = (double) refEnd / analysisRate;
            f.hostSeconds = (double) (it->hostSample + (streamConsumed - it->streamStart)) / sr;
            f.confidence = std::min (m.confidence, bestCorr);
            f.kind = SyncKind::fingerprint;
            f.locked = true;
            f.serial = ++serial;
            fix.write (f);
            break;
        }
    }
}

double FingerprintClipSource::hostSecondsOfNewest() const
{
    for (auto it = recent.rbegin(); it != recent.rend(); ++it)
        if (it->streamStart <= streamConsumed - 1)
            return (double) (it->hostSample + (streamConsumed - it->streamStart)) / sr;
    return -1.0;
}

void FingerprintClipSource::evaluateAligned()
{
    // Several takes recorded over the same stretch of the timeline (Live's take lanes keep each one where it was
    // recorded) differ only in which one is playing, not where. So compare the last 100 ms of what the track plays with
    // each candidate at the position it was recorded, and keep the best, with hysteresis so near equals never flip.
    const auto t0 = std::chrono::steady_clock::now();
    lastAlignedAt = analyzer.samples8kProduced();

    constexpr int window = 800;    // 100 ms at 8 kHz
    constexpr int step = 2;        // every other sample: the same answer for half the work
    constexpr int search = 4;      // lags tried around the current one, in 8 kHz samples
    constexpr float minCorr = 0.5f, poorCorr = 0.3f, margin = 1.25f;

    auto unlock = [this]
    {
        auto f = alignedFix.read();
        if (f.locked)
        {
            f.locked = false;
            f.serial = ++alignedSerial;
            alignedFix.write (f);
        }
    };

    auto spent = [this, t0] { alignedCpu = alignedCpu.load() + std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count(); };

    const auto& live = analyzer.recent8k();
    const auto host = hostSecondsOfNewest();

    if (candidates.size() < 2 || contiguous8k < 160 || (int) live.size() < window || host < 0.0)
    {
        unlock();
        winner = challenger = -1;
        poorWindows = 0;
        spent();
        return;
    }

    // Compare only from where the sound starts, so the first window after a silence or a start is not diluted by it.
    const float* W = live.data() + live.size() - (size_t) window;
    int skip = window - (int) std::min<int64_t> (window, contiguous8k);
    while (skip < window && std::abs (W[skip]) < 1.0e-4f)
        ++skip;
    skip += skip % step;
    const int length = window - skip;
    const float* L = W + skip;

    double ea = 0.0;
    for (int i = 0; i < length; i += step)
        ea += (double) L[i] * L[i];

    // Silence says nothing about which take plays: keep the current choice. After a long silence (a gap, a rest),
    // the next sound is judged afresh rather than against the take before it.
    if (length < 160 || ea / (length / step) < 1.0e-8)
    {
        if (++silentWindows >= 6)
        {
            winner = challenger = -1;
            challengerWins = 0;
        }

        spent();
        return;
    }

    silentWindows = 0;

    struct Score { int index; float corr; int lagFound; double takeSeconds; };
    std::vector<Score> scores;
    scores.reserve (candidates.size());

    for (int i = 0; i < (int) candidates.size(); ++i)
    {
        const auto& c = candidates[(size_t) i];
        if (c.index == nullptr || host < c.hostStart || host > c.hostEnd)
            continue;

        const auto& ref = c.index->audio8k;
        const auto t = c.takeAtHostStart + (host - c.hostStart) + latencyDelta;
        const auto refEnd = (int64_t) std::llround (t * analysisRate) + lag;
        float best = -2.0f;
        int bestD = 0;

        for (int d = -search; d <= search; ++d)
        {
            const auto start = refEnd + d - length;
            if (start < 0 || start + length > (int64_t) ref.size())
                continue;

            const auto* R = ref.data() + start;
            double num = 0.0, eb = 0.0;
            for (int k = 0; k < length; k += step)
            {
                const auto b = (double) R[k];
                num += (double) L[k] * b;
                eb += b * b;
            }

            const auto corr = (float) (num / std::sqrt (ea * eb + 1.0e-6));
            if (corr > best)
            {
                best = corr;
                bestD = d;
            }
        }

        if (best > -2.0f)
            scores.push_back ({ i, best, bestD, t + (double) (lag + bestD) / analysisRate });
    }

    if (scores.size() < 2)
    {
        // With one candidate nothing changes from before: the fingerprint and the prior handle it.
        unlock();
        winner = challenger = -1;
        poorWindows = 0;
        spent();
        return;
    }

    const auto top = std::max_element (scores.begin(), scores.end(), [] (const Score& a, const Score& b) { return a.corr < b.corr; });

    // Nothing here sounds like any take where it was recorded: a piece was moved. Old votes would only slow the
    // fingerprint down finding it, so start them afresh, once.
    if (top->corr < poorCorr)
    {
        if (++poorWindows >= 4)
        {
            if (wasGood)
                matcher.clearVotes();
            wasGood = false;
            winner = challenger = -1;
            unlock();
        }

        spent();
        return;
    }

    poorWindows = 0;

    auto publish = [&] (const Score& sc)
    {
        const auto& c = candidates[(size_t) sc.index];
        ClipFix f;
        f.takeId = c.takeId;
        f.pass = (int16_t) c.pass;
        f.hostSeconds = host;
        f.takeSeconds = sc.takeSeconds;
        f.confidence = sc.corr;
        f.kind = SyncKind::fingerprint;
        f.locked = true;
        f.serial = ++alignedSerial;
        alignedFix.write (f);
    };

    const Score* current = nullptr;
    for (const auto& sc : scores)
        if (sc.index == winner)
            current = &sc;

    if (current == nullptr)
    {
        // A first choice. Near equals (within the margin) go to the newest take, so identical takes pick the newest.
        if (top->corr < minCorr)
        {
            spent();
            return;
        }

        const Score* pick = &*top;
        for (const auto& sc : scores)
            if (sc.corr * margin >= top->corr && candidates[(size_t) sc.index].takeId > candidates[(size_t) pick->index].takeId)
                pick = &sc;

        winner = pick->index;
        challenger = -1;
        challengerWins = 0;
        current = pick;
    }
    else
    {
        // A challenger must beat the current take by the margin in two windows running.
        const Score* rival = nullptr;
        for (const auto& sc : scores)
            if (sc.index != winner && (rival == nullptr || sc.corr > rival->corr))
                rival = &sc;

        if (rival != nullptr && rival->corr >= minCorr && rival->corr > margin * std::max (current->corr, 0.05f))
        {
            challengerWins = rival->index == challenger ? challengerWins + 1 : 1;
            challenger = rival->index;

            if (challengerWins >= 2)
            {
                winner = rival->index;
                challenger = -1;
                challengerWins = 0;
                current = rival;
            }
        }
        else
        {
            challenger = -1;
            challengerWins = 0;
        }
    }

    if (current->corr >= 0.6f)
        wasGood = true;

    // Follow the latency the host adds, one sample at a time, and only from a take that clearly plays: around a cut
    // the old take's best lag is noise.
    if (current->corr >= 0.85f && current->lagFound != 0)
        lag = std::clamp (lag + (current->lagFound > 0 ? 1 : -1), -80, 80);

    publish (*current);
    spent();
}

} // namespace griddy::fingerprint
