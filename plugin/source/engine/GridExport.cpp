#include "GridExport.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#ifdef GRIDDY_HAVE_FFMPEG
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace griddy
{

//==============================================================================
GridExporter::GridExporter() : juce::Thread ("Griddy export") {}

GridExporter::~GridExporter()
{
    juce::String ignored;
    finish (ignored);
}

bool GridExporter::start (const ExportSettings& s, juce::String& error)
{
    if (running.load())
    {
        error = "an export is already running";
        return false;
    }

    settings = s;
    settings.width = std::max (2, s.width & ~1);
    settings.height = std::max (2, s.height & ~1);
    settings.fps = std::max (1, s.fps);

    backend = createExportBackend();
    if (backend == nullptr)
    {
        error = "this build cannot write MP4 files";
        return false;
    }

    settings.file.getParentDirectory().createDirectory();
    settings.file.deleteFile();

    if (! backend->open (settings, error))
    {
        backend.reset();
        return false;
    }

    ring = std::make_unique<SpscRing<float>> ((size_t) (settings.sampleRate * 2.0 * 30.0));
    pushed = 0;
    written = 0;
    framesOut = 0;
    videoDrops = 0;
    problem.clear();
    {
        const std::scoped_lock sl (pictureLock);
        pictures.clear();
    }
    finishing = false;
    running = true;
    startThread();
    return true;
}

void GridExporter::pushAudio (const float* left, const float* right, int n) noexcept
{
    if (! running.load (std::memory_order_acquire) || finishing.load (std::memory_order_relaxed))
        return;

    float tmp[1024];
    for (int done = 0; done < n;)
    {
        const auto m = std::min (n - done, 512);
        for (int i = 0; i < m; ++i)
        {
            tmp[2 * i] = left[done + i];
            tmp[2 * i + 1] = (right != nullptr ? right : left)[done + i];
        }

        ring->write (tmp, (size_t) m * 2);
        done += m;
    }

    pushed.fetch_add (n, std::memory_order_release);
}

double GridExporter::clockSeconds() const noexcept
{
    return (double) pushed.load (std::memory_order_acquire) / settings.sampleRate;
}

void GridExporter::pushVideo (const juce::Image& image, double pts)
{
    if (! running.load() || finishing.load() || ! image.isValid())
        return;

    {
        const std::scoped_lock sl (pictureLock);
        if (pictures.size() >= 8)
        {
            ++videoDrops;
            return;
        }
    }

    // The grid is drawn at the size of the canvas; the file is that size too.
    juce::Image::BitmapData in (image, juce::Image::BitmapData::readOnly);
    Picture p;
    p.pts = pts;
    p.bgra.resize ((size_t) settings.width * (size_t) settings.height * 4, 0);

    const auto w = std::min (settings.width, in.width), h = std::min (settings.height, in.height);
    for (int y = 0; y < h; ++y)
        std::memcpy (p.bgra.data() + (size_t) y * (size_t) settings.width * 4, in.getLinePointer (y), (size_t) w * 4);

    const std::scoped_lock sl (pictureLock);
    pictures.push_back (std::move (p));
}

bool GridExporter::drain (bool everything)
{
    bool didSomething = false;

    // Audio first, in blocks, so the encoder never waits on a picture to get its sound.
    constexpr size_t blockFrames = 2048;
    chunk.resize (blockFrames * 2);
    pcm.resize (blockFrames * 2);

    for (;;)
    {
        const auto avail = ring->available() / 2;
        if (avail == 0 || (! everything && avail < blockFrames))
            break;

        const auto frames = std::min (avail, blockFrames);
        const auto got = ring->read (chunk.data(), frames * 2) / 2;
        for (size_t i = 0; i < got * 2; ++i)
            pcm[i] = (int16_t) std::lround (juce::jlimit (-1.0f, 1.0f, chunk[i]) * 32767.0f);

        if (! backend->writeAudio (pcm.data(), (int) got, written))
            problem = "audio write failed";

        written += (int64_t) got;
        didSomething = true;
    }

    for (;;)
    {
        Picture p;
        {
            const std::scoped_lock sl (pictureLock);
            if (pictures.empty())
                break;

            p = std::move (pictures.front());
            pictures.pop_front();
        }

        if (backend->writeVideo (p.bgra.data(), settings.width * 4, p.pts))
            ++framesOut;

        didSomething = true;
    }

    return didSomething;
}

void GridExporter::run()
{
    while (! threadShouldExit())
    {
        if (! drain (false))
            wait (5);
    }
}

bool GridExporter::finish (juce::String& error)
{
    if (! running.load())
        return true;

    finishing = true;
    signalThreadShouldExit();
    stopThread (10000);
    drain (true);

    juce::String closeError;
    const auto ok = backend->close (closeError);
    backend.reset();
    running = false;

    if (problem.isNotEmpty())
        error = problem;
    else if (! ok)
        error = closeError;

    return ok && problem.isEmpty();
}

//==============================================================================
#ifdef GRIDDY_HAVE_FFMPEG

namespace
{
juce::String avErr (int code)
{
    char buf[128] = {};
    av_strerror (code, buf, sizeof (buf));
    return juce::String (buf);
}

class FfmpegBackend final : public ExportBackend
{
public:
    ~FfmpegBackend() override { release(); }

    bool open (const ExportSettings& s, juce::String& error) override
    {
        cfg = s;
        auto path = s.file.getFullPathName();
        int rc = avformat_alloc_output_context2 (&oc, nullptr, "mp4", path.toRawUTF8());
        if (rc < 0 || oc == nullptr)
            return fail (error, "alloc", rc);

        // Picture
        const AVCodec* vc = avcodec_find_encoder_by_name ("libx264");
        if (vc == nullptr)
            vc = avcodec_find_encoder (AV_CODEC_ID_H264);
        if (vc == nullptr)
            vc = avcodec_find_encoder (AV_CODEC_ID_MPEG4);
        if (vc == nullptr)
            return fail (error, "no video encoder", AVERROR (ENOENT));

        vs = avformat_new_stream (oc, nullptr);
        venc = avcodec_alloc_context3 (vc);
        venc->width = s.width;
        venc->height = s.height;
        venc->pix_fmt = AV_PIX_FMT_YUV420P;
        venc->time_base = { 1, s.fps };
        venc->framerate = { s.fps, 1 };
        venc->gop_size = s.fps * 2;
        venc->bit_rate = (int64_t) s.width * s.height * 4;
        if (oc->oformat->flags & AVFMT_GLOBALHEADER)
            venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        if (juce::String (vc->name) == "libx264")
        {
            av_opt_set (venc->priv_data, "preset", "veryfast", 0);
            av_opt_set (venc->priv_data, "crf", "20", 0);
            venc->bit_rate = 0;
        }

        rc = avcodec_open2 (venc, vc, nullptr);
        if (rc < 0)
            return fail (error, "open video encoder", rc);

        avcodec_parameters_from_context (vs->codecpar, venc);
        vs->time_base = venc->time_base;

        // Sound
        const AVCodec* ac = avcodec_find_encoder (AV_CODEC_ID_AAC);
        if (ac == nullptr)
            return fail (error, "no AAC encoder", AVERROR (ENOENT));

        as = avformat_new_stream (oc, nullptr);
        aenc = avcodec_alloc_context3 (ac);
        aenc->sample_rate = (int) s.sampleRate;
        aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default (&aenc->ch_layout, 2);
        aenc->bit_rate = 192000;
        aenc->time_base = { 1, (int) s.sampleRate };
        if (oc->oformat->flags & AVFMT_GLOBALHEADER)
            aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        rc = avcodec_open2 (aenc, ac, nullptr);
        if (rc < 0)
            return fail (error, "open AAC encoder", rc);

        avcodec_parameters_from_context (as->codecpar, aenc);
        as->time_base = aenc->time_base;

        rc = avio_open (&oc->pb, path.toRawUTF8(), AVIO_FLAG_WRITE);
        if (rc < 0)
            return fail (error, "open file", rc);

        AVDictionary* opts = nullptr;
        av_dict_set (&opts, "movflags", "+faststart", 0);
        rc = avformat_write_header (oc, &opts);
        av_dict_free (&opts);
        if (rc < 0)
            return fail (error, "header", rc);

        headerWritten = true;
        sws = sws_getContext (s.width, s.height, AV_PIX_FMT_BGRA, s.width, s.height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
        vframe = av_frame_alloc();
        vframe->format = AV_PIX_FMT_YUV420P;
        vframe->width = s.width;
        vframe->height = s.height;
        av_frame_get_buffer (vframe, 0);
        aframe = av_frame_alloc();
        pkt = av_packet_alloc();
        frameSize = aenc->frame_size > 0 ? aenc->frame_size : 1024;
        return sws != nullptr;
    }

    bool writeVideo (const uint8_t* bgra, int stride, double pts) override
    {
        const auto n = (int64_t) std::llround (pts * cfg.fps);
        if (n <= lastVideo)
            return false; // two pictures in one frame slot: keep the first

        av_frame_make_writable (vframe);
        const uint8_t* src[1] = { bgra };
        const int srcStride[1] = { stride };
        sws_scale (sws, src, srcStride, 0, cfg.height, vframe->data, vframe->linesize);
        vframe->pts = n;
        lastVideo = n;
        return encode (venc, vframe, vs);
    }

    bool writeAudio (const int16_t* stereo, int frames, int64_t) override
    {
        for (int i = 0; i < frames; ++i)
        {
            pendingL.push_back ((float) stereo[2 * i] / 32768.0f);
            pendingR.push_back ((float) stereo[2 * i + 1] / 32768.0f);
        }

        bool ok = true;
        while ((int) pendingL.size() - pendingStart >= frameSize)
            ok = sendAudio (frameSize) && ok;

        if (pendingStart > 65536)
        {
            pendingL.erase (pendingL.begin(), pendingL.begin() + pendingStart);
            pendingR.erase (pendingR.begin(), pendingR.begin() + pendingStart);
            pendingStart = 0;
        }

        return ok;
    }

    bool close (juce::String& error) override
    {
        bool ok = true;
        if (oc != nullptr && headerWritten)
        {
            const auto left = (int) pendingL.size() - pendingStart;
            if (left > 0)
                ok = sendAudio (left) && ok;

            ok = encode (venc, nullptr, vs) && ok;
            ok = encode (aenc, nullptr, as) && ok;
            const auto rc = av_write_trailer (oc);
            if (rc < 0)
            {
                error = "trailer: " + avErr (rc);
                ok = false;
            }
        }

        release();
        return ok;
    }

private:
    bool fail (juce::String& error, const char* what, int code)
    {
        error = juce::String (what) + ": " + avErr (code);
        release();
        return false;
    }

    bool sendAudio (int count)
    {
        aframe->nb_samples = count;
        aframe->format = AV_SAMPLE_FMT_FLTP;
        aframe->sample_rate = (int) cfg.sampleRate;
        av_channel_layout_default (&aframe->ch_layout, 2);
        if (av_frame_get_buffer (aframe, 0) < 0)
            return false;

        std::memcpy (aframe->data[0], pendingL.data() + pendingStart, (size_t) count * sizeof (float));
        std::memcpy (aframe->data[1], pendingR.data() + pendingStart, (size_t) count * sizeof (float));
        aframe->pts = audioPts;
        audioPts += count;
        pendingStart += count;

        const auto ok = encode (aenc, aframe, as);
        av_frame_unref (aframe);
        return ok;
    }

    bool encode (AVCodecContext* ctx, AVFrame* frame, AVStream* stream)
    {
        if (avcodec_send_frame (ctx, frame) < 0)
            return false;

        for (;;)
        {
            const auto rc = avcodec_receive_packet (ctx, pkt);
            if (rc == AVERROR (EAGAIN) || rc == AVERROR_EOF)
                return true;
            if (rc < 0)
                return false;

            av_packet_rescale_ts (pkt, ctx->time_base, stream->time_base);
            pkt->stream_index = stream->index;
            const auto w = av_interleaved_write_frame (oc, pkt);
            av_packet_unref (pkt);
            if (w < 0)
                return false;
        }
    }

    void release()
    {
        if (sws != nullptr) { sws_freeContext (sws); sws = nullptr; }
        if (vframe != nullptr) av_frame_free (&vframe);
        if (aframe != nullptr) av_frame_free (&aframe);
        if (pkt != nullptr) av_packet_free (&pkt);
        if (venc != nullptr) avcodec_free_context (&venc);
        if (aenc != nullptr) avcodec_free_context (&aenc);
        if (oc != nullptr)
        {
            if (oc->pb != nullptr)
                avio_closep (&oc->pb);
            avformat_free_context (oc);
            oc = nullptr;
        }
    }

    ExportSettings cfg;
    AVFormatContext* oc = nullptr;
    AVStream* vs = nullptr;
    AVStream* as = nullptr;
    AVCodecContext* venc = nullptr;
    AVCodecContext* aenc = nullptr;
    SwsContext* sws = nullptr;
    AVFrame* vframe = nullptr;
    AVFrame* aframe = nullptr;
    AVPacket* pkt = nullptr;
    bool headerWritten = false;
    int64_t lastVideo = -1, audioPts = 0;
    int frameSize = 1024;
    std::vector<float> pendingL, pendingR;
    int pendingStart = 0;
};
} // namespace

std::unique_ptr<ExportBackend> createExportBackend() { return std::make_unique<FfmpegBackend>(); }
bool gridExportAvailable() { return true; }

MediaInfo probeMedia (const juce::File& file)
{
    MediaInfo info;
    AVFormatContext* fc = nullptr;
    auto rc = avformat_open_input (&fc, file.getFullPathName().toRawUTF8(), nullptr, nullptr);
    if (rc < 0)
    {
        info.problem = "open: " + avErr (rc);
        return info;
    }

    avformat_find_stream_info (fc, nullptr);
    info.fileSeconds = fc->duration > 0 ? (double) fc->duration / AV_TIME_BASE : 0.0;

    for (unsigned i = 0; i < fc->nb_streams; ++i)
    {
        auto* st = fc->streams[i];
        const auto seconds = st->duration > 0 ? (double) st->duration * av_q2d (st->time_base) : 0.0;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            info.width = st->codecpar->width;
            info.height = st->codecpar->height;
            info.videoSeconds = seconds;
            info.videoFrames = (int) st->nb_frames;
            info.videoCodec = avcodec_get_name (st->codecpar->codec_id);
        }
        else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
        {
            info.audioSeconds = seconds;
            info.audioChannels = st->codecpar->ch_layout.nb_channels;
            info.audioCodec = avcodec_get_name (st->codecpar->codec_id);
        }
    }

    // Count the pictures by reading the packets when the container does not say.
    if (info.videoFrames == 0)
    {
        AVPacket* p = av_packet_alloc();
        int vidx = -1;
        for (unsigned i = 0; i < fc->nb_streams; ++i)
            if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                vidx = (int) i;
        while (av_read_frame (fc, p) >= 0)
        {
            if (p->stream_index == vidx)
                ++info.videoFrames;
            av_packet_unref (p);
        }
        av_packet_free (&p);
    }

    avformat_close_input (&fc);
    info.ok = info.width > 0 && info.audioChannels > 0;
    return info;
}

#elif JUCE_MAC
// createExportBackend, gridExportAvailable and probeMedia live in AVFoundationExportGrid.mm
#else

std::unique_ptr<ExportBackend> createExportBackend() { return nullptr; }
bool gridExportAvailable() { return false; }
MediaInfo probeMedia (const juce::File&) { MediaInfo i; i.problem = "built without FFmpeg"; return i; }

#endif

} // namespace griddy
