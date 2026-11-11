#include "MovTake.h"

#ifdef GRIDDY_HAVE_FFMPEG
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#endif

namespace griddy
{
#ifndef GRIDDY_HAVE_FFMPEG

bool ffmpegAvailable() { return false; }
std::unique_ptr<VideoDecoder> createFfmpegDecoder() { return nullptr; }

#if JUCE_MAC
bool exportTakeMovApple (const TakeMeta&, const juce::File&, const juce::File&, juce::String&); // AVFoundationExport.mm
bool movExportAvailable() { return true; }
bool exportTakeMov (const TakeMeta& m, const juce::File& f, const juce::File& o, juce::String& e) { return exportTakeMovApple (m, f, o, e); }
#else
bool movExportAvailable() { return false; }
bool exportTakeMov (const TakeMeta&, const juce::File&, const juce::File&, juce::String& error) { error = "built without FFmpeg"; return false; }
#endif

#else

bool ffmpegAvailable() { return true; }
bool movExportAvailable() { return true; }

namespace
{
juce::String avError (int code)
{
    char buf[128] = {};
    av_strerror (code, buf, sizeof (buf));
    return juce::String (buf);
}
} // namespace

bool exportTakeMov (const TakeMeta& meta, const juce::File& takeFolder, const juce::File& movFile, juce::String& error)
{
    if (meta.numFrames <= 0 || meta.frameTimes.size() < (size_t) meta.numFrames)
    {
        error = "take has no frames";
        return false;
    }

    std::vector<float> audio;
    double sr = 0.0;
    if (! readWav (takeFolder.getChildFile ("audio.wav"), audio, sr) || sr <= 0.0)
    {
        error = "no audio.wav";
        return false;
    }

    const auto tmp = movFile.getSiblingFile (movFile.getFileName() + ".part");
    tmp.deleteFile();

    AVFormatContext* oc = nullptr;
    int rc = avformat_alloc_output_context2 (&oc, nullptr, "mov", tmp.getFullPathName().toRawUTF8());
    if (rc < 0 || oc == nullptr)
    {
        error = "alloc: " + avError (rc);
        return false;
    }

    auto fail = [&] (const juce::String& what, int code)
    {
        error = what + ": " + avError (code);
        if (oc != nullptr)
        {
            if (oc->pb != nullptr)
                avio_closep (&oc->pb);
            avformat_free_context (oc);
            oc = nullptr;
        }
        tmp.deleteFile();
        return false;
    };

    const AVRational videoTb { 1, 30000 };
    auto* vs = avformat_new_stream (oc, nullptr);
    auto* as = avformat_new_stream (oc, nullptr);
    if (vs == nullptr || as == nullptr)
        return fail ("new stream", AVERROR (ENOMEM));

    vs->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    vs->codecpar->codec_id = AV_CODEC_ID_MJPEG;
    vs->codecpar->width = meta.width;
    vs->codecpar->height = meta.height;
    vs->codecpar->format = AV_PIX_FMT_YUVJ420P;
    vs->time_base = videoTb;

    as->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    as->codecpar->codec_id = AV_CODEC_ID_PCM_S16LE;
    as->codecpar->sample_rate = (int) sr;
    as->codecpar->format = AV_SAMPLE_FMT_S16;
    av_channel_layout_default (&as->codecpar->ch_layout, 1);
    as->codecpar->bits_per_coded_sample = 16;
    as->codecpar->block_align = 2;
    as->time_base = { 1, (int) sr };

    rc = avio_open (&oc->pb, tmp.getFullPathName().toRawUTF8(), AVIO_FLAG_WRITE);
    if (rc < 0)
        return fail ("open", rc);

    // Delay both streams together if the first picture would land before zero.
    const auto firstPicture = meta.frameTimes.front() + meta.videoOffsetSeconds();
    const auto lead = std::max (0.0, -firstPicture);

    rc = avformat_write_header (oc, nullptr);
    if (rc < 0)
        return fail ("header", rc);

    const auto vtb = vs->time_base, atb = as->time_base;
    int vi = 0;
    int64_t aPos = 0, lastVideoPts = -1;
    const int chunk = 2048;
    const auto leadSamples = (int64_t) std::llround (lead * sr);
    std::vector<int16_t> pcm ((size_t) chunk);

    auto videoTime = [&] (int i) { return meta.frameTimes[(size_t) i] + meta.videoOffsetSeconds() + lead; };
    auto audioTime = [&] (int64_t pos) { return (double) (pos + leadSamples) / sr; };

    const int64_t totalAudio = (int64_t) audio.size();
    auto* pkt = av_packet_alloc();

    while (vi < meta.numFrames || aPos < totalAudio)
    {
        const bool takeVideo = vi < meta.numFrames && (aPos >= totalAudio || videoTime (vi) <= audioTime (aPos));

        if (takeVideo)
        {
            juce::MemoryBlock jpeg;
            if (! takeFolder.getChildFile ("frames").getChildFile (juce::String::formatted ("%06d.jpg", vi)).loadFileAsData (jpeg) || jpeg.getSize() == 0)
            {
                av_packet_free (&pkt);
                return fail ("missing frame " + juce::String (vi), AVERROR (ENOENT));
            }

            av_new_packet (pkt, (int) jpeg.getSize());
            std::memcpy (pkt->data, jpeg.getData(), jpeg.getSize());
            auto pts = (int64_t) std::llround (videoTime (vi) * videoTb.den / videoTb.num);
            if (pts <= lastVideoPts)
                pts = lastVideoPts + 1;
            lastVideoPts = pts;
            pkt->pts = pkt->dts = pts;
            pkt->duration = 1000;
            pkt->flags |= AV_PKT_FLAG_KEY;
            pkt->stream_index = vs->index;
            av_packet_rescale_ts (pkt, videoTb, vtb);
            rc = av_interleaved_write_frame (oc, pkt);
            ++vi;
        }
        else
        {
            const auto n = (int) std::min<int64_t> (chunk, totalAudio - aPos);
            for (int i = 0; i < n; ++i)
                pcm[(size_t) i] = (int16_t) std::clamp (std::lround (audio[(size_t) (aPos + i)] * 32767.0f), -32768l, 32767l);

            av_new_packet (pkt, n * 2);
            std::memcpy (pkt->data, pcm.data(), (size_t) n * 2);
            pkt->pts = pkt->dts = aPos + leadSamples;
            pkt->duration = n;
            pkt->flags |= AV_PKT_FLAG_KEY;
            pkt->stream_index = as->index;
            av_packet_rescale_ts (pkt, AVRational { 1, (int) sr }, atb);
            rc = av_interleaved_write_frame (oc, pkt);
            aPos += n;
        }

        if (rc < 0)
        {
            av_packet_free (&pkt);
            return fail ("write", rc);
        }
    }

    av_packet_free (&pkt);
    rc = av_write_trailer (oc);
    avio_closep (&oc->pb);
    avformat_free_context (oc);
    oc = nullptr;

    if (rc < 0)
    {
        tmp.deleteFile();
        error = "trailer: " + avError (rc);
        return false;
    }

    movFile.deleteFile();
    return tmp.moveFileTo (movFile);
}

//==============================================================================
namespace
{
class FfmpegDecoder final : public VideoDecoder
{
public:
    ~FfmpegDecoder() override { close(); }

    bool open (const TakeMeta& meta, const juce::File& takeFolder, bool useProxy) override
    {
        av_log_set_level (AV_LOG_ERROR);
        const auto path = takeFolder.getChildFile ("take.mov").getFullPathName();
        if (avformat_open_input (&fmt, path.toRawUTF8(), nullptr, nullptr) < 0)
            return false;

        if (avformat_find_stream_info (fmt, nullptr) < 0)
            return close(), false;

        stream = av_find_best_stream (fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (stream < 0)
            return close(), false;

        auto* par = fmt->streams[stream]->codecpar;
        const auto* codec = avcodec_find_decoder (par->codec_id);
        if (codec == nullptr)
            return close(), false;

        ctx = avcodec_alloc_context3 (codec);
        avcodec_parameters_to_context (ctx, par);
        ctx->thread_count = 1;
        if (avcodec_open2 (ctx, codec, nullptr) < 0)
            return close(), false;

        // Frame timestamps from the container's index: no need to read the file to know where frames are.
        auto* st = fmt->streams[stream];
        const auto n = avformat_index_get_entries_count (st);
        for (int i = 0; i < n; ++i)
            stamps.push_back (avformat_index_get_entry (st, i)->timestamp);
        std::sort (stamps.begin(), stamps.end());

        outW = useProxy ? meta.proxyWidth : ctx->width;
        outH = useProxy ? meta.proxyHeight : ctx->height;
        frame = av_frame_alloc();
        packet = av_packet_alloc();
        return ! stamps.empty();
    }

    int numFrames() const override { return (int) stamps.size(); }

    juce::Image decode (int index) override
    {
        if (index < 0 || index >= (int) stamps.size() || fmt == nullptr)
            return {};

        const auto ts = stamps[(size_t) index];
        if (index != lastIndex + 1)
        {
            av_seek_frame (fmt, stream, ts, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers (ctx);
        }

        lastIndex = index;

        for (int guard = 0; guard < 64; ++guard)
        {
            const auto rc = av_read_frame (fmt, packet);
            if (rc < 0)
                return {};

            if (packet->stream_index != stream)
            {
                av_packet_unref (packet);
                continue;
            }

            avcodec_send_packet (ctx, packet);
            av_packet_unref (packet);

            while (avcodec_receive_frame (ctx, frame) == 0)
            {
                if (frame->best_effort_timestamp >= ts)
                {
                    auto img = convert();
                    av_frame_unref (frame);
                    return img;
                }

                av_frame_unref (frame);
            }
        }

        return {};
    }

    const char* name() const override { return "FFmpeg software (take.mov)"; }

private:
    juce::Image convert()
    {
        // JPEG frames are full range YUV. Say so with the range flag instead of the deprecated J pixel formats.
        auto srcFormat = (AVPixelFormat) frame->format;
        bool fullRange = frame->color_range == AVCOL_RANGE_JPEG;
        switch (srcFormat)
        {
            case AV_PIX_FMT_YUVJ420P: srcFormat = AV_PIX_FMT_YUV420P; fullRange = true; break;
            case AV_PIX_FMT_YUVJ422P: srcFormat = AV_PIX_FMT_YUV422P; fullRange = true; break;
            case AV_PIX_FMT_YUVJ444P: srcFormat = AV_PIX_FMT_YUV444P; fullRange = true; break;
            default: break;
        }

        sws = sws_getCachedContext (sws, frame->width, frame->height, srcFormat,
                                    outW, outH, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws == nullptr)
            return {};

        sws_setColorspaceDetails (sws, sws_getCoefficients (SWS_CS_DEFAULT), fullRange ? 1 : 0,
                                  sws_getCoefficients (SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);

        juce::Image img (juce::Image::ARGB, outW, outH, false, juce::SoftwareImageType());
        const juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
        uint8_t* dst[4] = { bd.data, nullptr, nullptr, nullptr };
        int dstStride[4] = { bd.lineStride, 0, 0, 0 };
        sws_scale (sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
        return img;
    }

    void close()
    {
        if (sws != nullptr) { sws_freeContext (sws); sws = nullptr; }
        if (frame != nullptr) av_frame_free (&frame);
        if (packet != nullptr) av_packet_free (&packet);
        if (ctx != nullptr) avcodec_free_context (&ctx);
        if (fmt != nullptr) avformat_close_input (&fmt);
    }

    AVFormatContext* fmt = nullptr;
    AVCodecContext* ctx = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* sws = nullptr;
    int stream = -1, outW = 0, outH = 0, lastIndex = -10;
    std::vector<int64_t> stamps;
};
} // namespace

std::unique_ptr<VideoDecoder> createFfmpegDecoder() { return std::make_unique<FfmpegDecoder>(); }

#endif
} // namespace griddy
