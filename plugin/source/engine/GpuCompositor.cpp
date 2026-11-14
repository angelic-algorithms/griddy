#include "GpuCompositor.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

#include <atomic>
#include <map>

namespace griddy
{
using namespace juce::gl;

struct GpuCompositor::Impl : public juce::Component
{
    juce::OpenGLContext context;
    std::unique_ptr<juce::OpenGLShaderProgram> program;
    std::unique_ptr<juce::OpenGLFrameBuffer> target;
    GLuint vbo = 0;
    int width = 0, height = 0;
    std::atomic<bool> ready { false };
    juce::String info;

    struct Tex
    {
        GLuint id = 0;
        const void* frameId = nullptr;
        int w = 0, h = 0;
    };

    std::map<int, Tex> textures;
    std::vector<uint8_t> readback;
    double drawMs = 0.0, readMs = 0.0;

    Impl()
    {
        setOpaque (true);
        setSize (16, 16);
        addToDesktop (juce::ComponentPeer::windowIsTemporary);
        setVisible (true);
        context.setComponentPaintingEnabled (false);
        context.setContinuousRepainting (false);
        context.setMultisamplingEnabled (false);
        context.attachTo (*this);
    }

    ~Impl() override { context.detach(); }

    // Runs on the GL thread.
    bool setUp (int w, int h)
    {
        width = w;
        height = h;
        target = std::make_unique<juce::OpenGLFrameBuffer>();
        if (! target->initialise (context, w, h))
            return false;

        program = std::make_unique<juce::OpenGLShaderProgram> (context);
        const juce::String vs = "attribute vec2 pos;\nattribute vec2 uv;\nvarying vec2 vUV;\nvoid main(){ vUV = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
        const juce::String fs = "uniform sampler2D tex;\nuniform vec4 solid;\nuniform float useSolid;\nvarying vec2 vUV;\n"
                                "void main(){ vec4 c = texture2D(tex, vUV); gl_FragColor = mix(c, solid, useSolid); }\n";

        if (! program->addVertexShader (juce::OpenGLHelpers::translateVertexShaderToV3 (vs))
            || ! program->addFragmentShader (juce::OpenGLHelpers::translateFragmentShaderToV3 (fs))
            || ! program->link())
            return false;

        glGenBuffers (1, &vbo);
        info = juce::String ((const char*) glGetString (GL_RENDERER)) + ", GL " + juce::String ((const char*) glGetString (GL_VERSION));
        return true;
    }

    void tearDown()
    {
        for (auto& [slot, t] : textures)
            if (t.id != 0)
                glDeleteTextures (1, &t.id);
        textures.clear();
        if (vbo != 0)
            glDeleteBuffers (1, &vbo);
        vbo = 0;
        program.reset();
        if (target != nullptr)
            target->release();
        target.reset();
    }

    void upload (int slot, const std::shared_ptr<const VideoFrame>& f)
    {
        auto& t = textures[slot];
        if (t.id == 0)
            glGenTextures (1, &t.id);

        if (t.frameId == f.get())
            return;

        t.frameId = f.get();
        const juce::Image::BitmapData bd (f->image, juce::Image::BitmapData::readOnly);
        glBindTexture (GL_TEXTURE_2D, t.id);
        glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei (GL_UNPACK_ROW_LENGTH, bd.lineStride / 4);

        if (t.w != bd.width || t.h != bd.height)
        {
            t.w = bd.width;
            t.h = bd.height;
            glTexImage2D (GL_TEXTURE_2D, 0, GL_RGBA8, bd.width, bd.height, 0, GL_BGRA, GL_UNSIGNED_BYTE, bd.data);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        else
        {
            glTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0, bd.width, bd.height, GL_BGRA, GL_UNSIGNED_BYTE, bd.data);
        }

        glPixelStorei (GL_UNPACK_ROW_LENGTH, 0);
    }

    // Runs on the GL thread.
    void draw (juce::Image& output, const std::vector<Cell>& cells, const Compositor::FrameForSlot& frameForSlot, float gap)
    {
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        target->makeCurrentRenderingTarget();
        glViewport (0, 0, width, height);
        glDisable (GL_BLEND);
        glDisable (GL_DEPTH_TEST);
        glClearColor (0x0b / 255.0f, 0x0b / 255.0f, 0x0f / 255.0f, 1.0f);
        glClear (GL_COLOR_BUFFER_BIT);

        program->use();
        const auto posLoc = (GLuint) glGetAttribLocation (program->getProgramID(), "pos");
        const auto uvLoc = (GLuint) glGetAttribLocation (program->getProgramID(), "uv");
        glActiveTexture (GL_TEXTURE0);
        glUniform1i (glGetUniformLocation (program->getProgramID(), "tex"), 0);
        const auto solidLoc = glGetUniformLocation (program->getProgramID(), "solid");
        const auto useSolidLoc = glGetUniformLocation (program->getProgramID(), "useSolid");

        glBindBuffer (GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray (posLoc);
        glEnableVertexAttribArray (uvLoc);
        glVertexAttribPointer (posLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof (float), nullptr);
        glVertexAttribPointer (uvLoc, 2, GL_FLOAT, GL_FALSE, 4 * sizeof (float), (const void*) (2 * sizeof (float)));

        const auto outW = (float) width, outH = (float) height, halfGap = gap * 0.5f;

        for (const auto& cell : cells)
        {
            auto area = juce::Rectangle<float> (cell.area.getX() * outW, cell.area.getY() * outH,
                                                cell.area.getWidth() * outW, cell.area.getHeight() * outH)
                            .reduced (cells.size() > 1 ? halfGap : 0.0f);
            if (area.isEmpty())
                continue;

            const auto px = area.toNearestInt();
            const auto frame = frameForSlot (cell.slot);
            float u0 = 0, v0 = 0, u1 = 1, v1 = 1;

            if (frame != nullptr && frame->image.isValid())
            {
                upload (cell.slot, frame);
                glBindTexture (GL_TEXTURE_2D, textures[cell.slot].id);
                const auto sw = (float) frame->image.getWidth(), sh = (float) frame->image.getHeight();
                const auto scale = std::max ((float) px.getWidth() / sw, (float) px.getHeight() / sh);
                const auto viewW = (float) px.getWidth() / scale, viewH = (float) px.getHeight() / scale;
                u0 = ((sw - viewW) * 0.5f) / sw;
                u1 = u0 + viewW / sw;
                v0 = ((sh - viewH) * 0.5f) / sh;
                v1 = v0 + viewH / sh;
                glUniform1f (useSolidLoc, 0.0f);
            }
            else
            {
                glUniform1f (useSolidLoc, 1.0f);
                glUniform4f (solidLoc, 0x1c / 255.0f, 0x1c / 255.0f, 0x24 / 255.0f, 1.0f);
            }

            // Pixel rect to clip space. The image is read back bottom row first, so texture v is flipped
            // instead of the output: y = 0 at the top of the picture maps to clip y = +1.
            const auto cx0 = (float) px.getX() / outW * 2.0f - 1.0f, cx1 = (float) px.getRight() / outW * 2.0f - 1.0f;
            const auto cy0 = 1.0f - (float) px.getY() / outH * 2.0f, cy1 = 1.0f - (float) px.getBottom() / outH * 2.0f;
            const float quad[] = { cx0, cy0, u0, v0,  cx1, cy0, u1, v0,  cx0, cy1, u0, v1,  cx1, cy1, u1, v1 };
            glBufferData (GL_ARRAY_BUFFER, sizeof (quad), quad, GL_STREAM_DRAW);
            glDrawArrays (GL_TRIANGLE_STRIP, 0, 4);
        }

        glDisableVertexAttribArray (posLoc);
        glDisableVertexAttribArray (uvLoc);
        glFinish();
        const auto t1 = juce::Time::getMillisecondCounterHiRes();

        // Read back. The framebuffer's row 0 is the bottom, so copy rows in reverse.
        readback.resize ((size_t) width * (size_t) height * 4);
        glPixelStorei (GL_PACK_ALIGNMENT, 1);
        glReadPixels (0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, readback.data());
        target->releaseAsRenderingTarget();

        const juce::Image::BitmapData bd (output, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < height; ++y)
            std::memcpy (bd.getLinePointer (y), readback.data() + (size_t) (height - 1 - y) * (size_t) width * 4, (size_t) width * 4);

        drawMs = t1 - t0;
        readMs = juce::Time::getMillisecondCounterHiRes() - t1;
    }
};

GpuCompositor::GpuCompositor() = default;
GpuCompositor::~GpuCompositor() { shutdown(); }

bool GpuCompositor::initialise (int w, int h)
{
    JUCE_ASSERT_MESSAGE_THREAD
    shutdown();
    impl = std::make_unique<Impl>();

    std::atomic<int> result { 0 };
    impl->context.executeOnGLThread ([this, w, h, &result] (juce::OpenGLContext&)
    {
        result = impl->setUp (w, h) ? 1 : -1;
    }, false);

    // The GL thread starts asynchronously; pump the message loop until it has run our job.
    const auto deadline = juce::Time::getMillisecondCounter() + 5000;
    while (result.load() == 0 && juce::Time::getMillisecondCounter() < deadline)
    {
        impl->context.triggerRepaint();
        juce::Thread::sleep (10);
    }

    if (result.load() != 1)
    {
        shutdown();
        return false;
    }

    impl->ready = true;
    return true;
}

bool GpuCompositor::available() const noexcept { return impl != nullptr && impl->ready.load(); }
juce::String GpuCompositor::description() const { return impl != nullptr ? impl->info : juce::String(); }
double GpuCompositor::lastDrawMs() const noexcept { return impl != nullptr ? impl->drawMs : 0.0; }
double GpuCompositor::lastReadbackMs() const noexcept { return impl != nullptr ? impl->readMs : 0.0; }

void GpuCompositor::render (juce::Image& output, const std::vector<Cell>& cells, const Compositor::FrameForSlot& frameForSlot)
{
    if (! available())
        return;

    juce::WaitableEvent done;
    impl->context.executeOnGLThread ([&] (juce::OpenGLContext&)
    {
        impl->draw (output, cells, frameForSlot, 4.0f);
        done.signal();
    }, false);

    impl->context.triggerRepaint();
    done.wait (2000);
}

void GpuCompositor::shutdown()
{
    if (impl == nullptr)
        return;

    impl->ready = false;
    juce::WaitableEvent done;
    impl->context.executeOnGLThread ([&] (juce::OpenGLContext&) { impl->tearDown(); done.signal(); }, false);
    impl->context.triggerRepaint();
    done.wait (2000);
    impl.reset();
}

} // namespace griddy
