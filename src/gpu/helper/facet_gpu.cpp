// facet-gpu: puts Facet's frames on the screen with the GPU. The core (a
// static binary that cannot load GPU drivers) starts this helper from the
// downloaded OpenGL package; the helper owns the display through DRM/KMS and
// flips buffers in step with it. The core sends the interface as drawing
// commands (gfx/ops.h), which are drawn here with OpenGL ES together with
// the plugin surfaces; older cores send a finished canvas instead.
//
//   facet-gpu --fd N --card /dev/dri/card0      on the screen
//   facet-gpu --fd N --offscreen WxH [--dump f]  without a screen (tests)
//
// The protocol is described in src/gpu/channel.h.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>  // after gl3.h, which defines its macros
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "gfx/ops.h"
#include "gpu/channel.h"

namespace ops = facet::gfx::ops;

using facet::Json;
using facet::gpu::Channel;

namespace {

volatile sig_atomic_t g_quit = 0;

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    static Rect from(const Json& a) { return {a[size_t(0)].as_int(), a[size_t(1)].as_int(), a[size_t(2)].as_int(), a[size_t(3)].as_int()}; }
};

struct Texture {
    GLuint id = 0;
    int w = 0, h = 0;
};

// A GPU buffer of a surface, imported once: drawn straight from the
// plugin's memory on the GPU.
struct GpuTexture {
    GLuint tex = 0;
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
};

struct Surface {
    const uint8_t* map = nullptr;
    size_t size = 0;
    int w = 0, h = 0, stride = 0, buffers = 0;
    Texture tex;
    int uploaded = -1;  // buffer now in the texture
    GpuTexture gpu[8];
};

const char* kVertex = R"(
attribute vec2 pos;
attribute vec2 uv;
varying vec2 v;
void main() { v = uv; gl_Position = vec4(pos, 0.0, 1.0); }
)";

// Facet's pixels are XRGB8888 in memory order B, G, R, A: uploaded as RGBA
// bytes and swizzled here. Surfaces are opaque; the canvas' alpha marks the
// holes where the surfaces under it show.
const char* kFragment = R"(
precision mediump float;
varying vec2 v;
uniform sampler2D tex;
uniform float opaque;
void main() {
    vec4 c = texture2D(tex, v).bgra;
    if (opaque > 0.5) c.a = 1.0;
    gl_FragColor = c;
}
)";

// Drawing commands: one quad per shape. Distances to the shape's and the
// clip's edges are interpolated per pixel (small numbers near the edges, so
// medium precision is enough), which gives the same anti-aliasing as the
// core's rasterizer: covered area for rectangles, distance for corners.
const char* kOpsVertex = R"(
attribute vec2 pos;
attribute vec2 uv;
attribute vec4 edge;
attribute vec4 clip;
attribute vec4 color;
attribute vec2 param;
uniform vec2 size;
varying vec2 v_uv;
varying vec4 v_edge;
varying vec4 v_clip;
varying vec4 v_color;
varying vec2 v_param;
void main() {
    v_uv = uv;
    v_edge = edge;
    v_clip = clip;
    v_color = color;
    v_param = param;
    gl_Position = vec4(pos.x / size.x * 2.0 - 1.0, 1.0 - pos.y / size.y * 2.0, 0.0, 1.0);
}
)";

const char* kOpsFragment = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
varying vec2 v_uv;
varying vec4 v_edge;
varying vec4 v_clip;
varying vec4 v_color;
varying vec2 v_param;
uniform sampler2D tex;
uniform float native;  // images: 1 = a GPU buffer (channels in order), 0 = Facet's B, G, R bytes
float span(float lo, float hi) { return clamp(min(lo, 0.5) + min(hi, 0.5), 0.0, 1.0); }
void main() {
    float cov = span(v_clip.x, v_clip.z) * span(v_clip.y, v_clip.w);
    vec4 col = v_color;
    if (v_param.y < 0.5) {
        float r = v_param.x;
        if (r < 0.5) {
            cov *= span(v_edge.x, v_edge.z) * span(v_edge.y, v_edge.w);
        } else {
            vec2 q = vec2(r) - min(v_edge.xy, v_edge.zw);
            float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
            cov *= clamp(0.5 - d, 0.0, 1.0);
        }
    } else if (v_param.y < 1.5) {
        cov *= texture2D(tex, v_uv).a;
    } else {
        vec3 t = texture2D(tex, v_uv).rgb;
        col = vec4(native > 0.5 ? t : t.bgr, 1.0);
    }
    gl_FragColor = vec4(col.rgb, col.a * cov);
}
)";

struct Vertex {
    float x, y, u, v;
    float edge[4];  // to the shape's left, top, right, bottom edge
    float clip[4];  // the same for the clip
    uint8_t color[4];
    float radius, mode;  // mode: 0 shape, 1 alpha mask, 2 image
};

// What a batch of quads samples.
struct TexRef {
    enum Kind { None, Page, Inline, Image, Layer } kind = None;
    int index = 0;
    bool operator==(const TexRef& o) const { return kind == o.kind && index == o.index; }
};

struct Batch {
    TexRef tex;
    size_t first = 0;  // vertex
    size_t quads = 0;
};

class Gpu {
public:
    explicit Gpu(Channel& ch) : ch_(ch) {}
    ~Gpu() { shutdown(); }

    bool init_kms(const char* card, std::string& error) {
        drm_ = ::open(card, O_RDWR | O_CLOEXEC);
        if (drm_ < 0) return fail(error, std::string("cannot open ") + card + ": " + std::strerror(errno));
        if (drmSetMaster(drm_) != 0) return fail(error, "cannot become DRM master (is another display server running?)");
        drmModeRes* res = drmModeGetResources(drm_);
        if (!res) return fail(error, "no KMS resources on " + std::string(card));
        drmModeConnector* best = nullptr;
        for (int i = 0; i < res->count_connectors; ++i) {
            drmModeConnector* c = drmModeGetConnector(drm_, res->connectors[i]);
            if (!c) continue;
            bool usable = c->connection == DRM_MODE_CONNECTED && c->count_modes > 0;
            bool internal = c->connector_type == DRM_MODE_CONNECTOR_eDP || c->connector_type == DRM_MODE_CONNECTOR_LVDS ||
                            c->connector_type == DRM_MODE_CONNECTOR_DSI;
            if (usable && (!best || (internal && best->connector_type != DRM_MODE_CONNECTOR_eDP))) {
                if (best) drmModeFreeConnector(best);
                best = c;
            } else {
                drmModeFreeConnector(c);
            }
        }
        if (!best) {
            drmModeFreeResources(res);
            return fail(error, "no connected display");
        }
        conn_ = best->connector_id;
        mode_ = best->modes[0];
        for (int i = 0; i < best->count_modes; ++i)
            if (best->modes[i].type & DRM_MODE_TYPE_PREFERRED) mode_ = best->modes[i];
        // A CRTC that can drive this connector.
        for (int e = 0; e < best->count_encoders && !crtc_; ++e) {
            drmModeEncoder* enc = drmModeGetEncoder(drm_, best->encoders[e]);
            if (!enc) continue;
            if (enc->encoder_id == best->encoder_id && enc->crtc_id) crtc_ = enc->crtc_id;
            for (int i = 0; i < res->count_crtcs && !crtc_; ++i)
                if (enc->possible_crtcs & (1u << i)) crtc_ = res->crtcs[i];
            drmModeFreeEncoder(enc);
        }
        dpms_prop_ = 0;
        if (drmModeObjectProperties* props = drmModeObjectGetProperties(drm_, conn_, DRM_MODE_OBJECT_CONNECTOR)) {
            for (uint32_t i = 0; i < props->count_props; ++i) {
                drmModePropertyRes* p = drmModeGetProperty(drm_, props->props[i]);
                if (p && std::strcmp(p->name, "DPMS") == 0) dpms_prop_ = p->prop_id;
                if (p) drmModeFreeProperty(p);
            }
            drmModeFreeObjectProperties(props);
        }
        drmModeFreeConnector(best);
        drmModeFreeResources(res);
        if (!crtc_) return fail(error, "no CRTC for the display");
        saved_ = drmModeGetCrtc(drm_, crtc_);
        w_ = mode_.hdisplay;
        h_ = mode_.vdisplay;

        gbm_ = gbm_create_device(drm_);
        if (!gbm_) return fail(error, "GBM is unavailable");
        gsurf_ = gbm_surface_create(gbm_, uint32_t(w_), uint32_t(h_), GBM_FORMAT_XRGB8888,
                                    GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (!gsurf_) return fail(error, "cannot create a GBM surface");
        auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        dpy_ = get_display ? get_display(EGL_PLATFORM_GBM_KHR, gbm_, nullptr) : eglGetDisplay(EGLNativeDisplayType(gbm_));
        return init_egl(true, error);
    }

    // Without a screen; on `device` (a render node) the real GPU draws, else
    // whatever Mesa has (tests).
    bool init_offscreen(int w, int h, const char* device, std::string& error) {
        w_ = w;
        h_ = h;
        auto get_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (!get_display) return fail(error, "EGL platform displays are unsupported");
        if (device) {
            render_fd_ = ::open(device, O_RDWR | O_CLOEXEC);
            if (render_fd_ < 0) return fail(error, std::string("cannot open ") + device);
            gbm_ = gbm_create_device(render_fd_);
            if (!gbm_) return fail(error, "GBM is unavailable");
            dpy_ = get_display(EGL_PLATFORM_GBM_KHR, gbm_, nullptr);
        } else {
            dpy_ = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
        }
        if (!init_egl(false, error, device != nullptr)) return false;
        glGenFramebuffers(1, &fbo_);
        glGenRenderbuffers(1, &rbo_);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, w_, h_);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rbo_);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return fail(error, "no offscreen target");
        return true;
    }

    int width() const { return w_; }
    int height() const { return h_; }
    int drm_fd() const { return drm_; }
    std::string renderer() const {
        const char* r = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        return r ? r : "?";
    }

    // The shared memory the core writes its drawing commands into.
    void set_ops(int fd, size_t size) {
        if (ops_map_) munmap(const_cast<uint8_t*>(ops_map_), ops_size_);
        void* m = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        ops_map_ = m == MAP_FAILED ? nullptr : static_cast<const uint8_t*>(m);
        ops_size_ = ops_map_ ? size : 0;
        verts_.clear();
        batches_.clear();
    }

    void set_canvas(int fd, int w, int h) {
        if (canvas_map_) munmap(const_cast<uint8_t*>(canvas_map_), canvas_size_);
        canvas_size_ = size_t(w) * size_t(h) * 4;
        void* m = mmap(nullptr, canvas_size_, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        canvas_map_ = m == MAP_FAILED ? nullptr : static_cast<const uint8_t*>(m);
        canvas_tex_ = make_texture(w, h, false);
        upload(canvas_tex_, canvas_map_, w * 4, {0, 0, w, h});
    }

    void add_surface(const std::string& key, int fd, int w, int h, int stride, int buffers) {
        drop_surface(key);
        Surface s;
        s.w = w, s.h = h, s.stride = stride, s.buffers = buffers;
        s.size = size_t(stride) * size_t(h) * size_t(buffers);
        void* m = mmap(nullptr, s.size, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (m == MAP_FAILED) return;
        s.map = static_cast<const uint8_t*>(m);
        s.tex = make_texture(w, h, true);
        surfaces_[key] = s;
    }

    void drop_surface(const std::string& key) {
        auto it = surfaces_.find(key);
        if (it == surfaces_.end()) return;
        munmap(const_cast<uint8_t*>(it->second.map), it->second.size);
        glDeleteTextures(1, &it->second.tex.id);
        for (auto& g : it->second.gpu) release(g);
        surfaces_.erase(it);
    }

    bool dmabuf_supported() const { return dmabuf_; }

    // A plugin's GPU buffer for slot `slot` of surface `key` (replaces the slot's old one).
    void add_gpu_buffer(const Json& m, int fd) {
        auto it = surfaces_.find(m["key"].str());
        int slot = m["slot"].as_int(-1);
        if (it == surfaces_.end() || slot < 0 || slot >= 8 || !dmabuf_) {
            ::close(fd);
            return;
        }
        GpuTexture& g = it->second.gpu[slot];
        release(g);
        uint64_t modifier = uint64_t(uint32_t(m["mod_hi"].as_number(0))) << 32 | uint32_t(m["mod_lo"].as_number(0));
        std::vector<EGLint> a = {EGL_WIDTH, m["w"].as_int(), EGL_HEIGHT, m["h"].as_int(), EGL_LINUX_DRM_FOURCC_EXT,
                                 EGLint(uint32_t(m["format"].as_number(0))), EGL_DMA_BUF_PLANE0_FD_EXT, fd,
                                 EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGLint(uint32_t(m["offset"].as_number(0))),
                                 EGL_DMA_BUF_PLANE0_PITCH_EXT, EGLint(uint32_t(m["stride"].as_number(0)))};
        constexpr uint64_t kInvalidModifier = 0x00ffffffffffffffull;
        if (dmabuf_modifiers_ && modifier != kInvalidModifier) {
            a.insert(a.end(), {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGLint(uint32_t(modifier)),
                               EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGLint(uint32_t(modifier >> 32))});
        }
        a.push_back(EGL_NONE);
        g.image = create_image_(dpy_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, a.data());
        ::close(fd);  // the image keeps the buffer
        if (g.image == EGL_NO_IMAGE_KHR) {
            std::fprintf(stderr, "facet-gpu: cannot import a GPU buffer (EGL error 0x%x)\n", eglGetError());
            return;
        }
        glGenTextures(1, &g.tex);
        glBindTexture(GL_TEXTURE_2D, g.tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        image_target_(GL_TEXTURE_2D, g.image);
    }

    void release(GpuTexture& g) {
        if (g.tex) glDeleteTextures(1, &g.tex);
        if (g.image != EGL_NO_IMAGE_KHR) destroy_image_(dpy_, g.image);
        g = GpuTexture{};
    }

    // Uploads what changed, draws, and starts showing the frame.
    void frame(const Json& msg) {
        if (ops_map_) return ops_frame(msg);
        int seq = msg["seq"].as_int();
        if (canvas_map_)
            for (const auto& r : msg["canvas"].items()) upload(canvas_tex_, canvas_map_, canvas_tex_.w * 4, Rect::from(r));
        struct Draw {
            Texture tex;
            Rect dst, clip;
        };
        std::vector<Draw> draws;
        for (const auto& l : msg["layers"].items()) {
            auto it = surfaces_.find(l["key"].str());
            if (it == surfaces_.end()) continue;
            Surface& s = it->second;
            int b = l["buffer"].as_int(-1);
            if (b < 0 || b >= s.buffers) continue;
            if (b != s.uploaded || l["fresh"].as_bool()) {
                upload(s.tex, s.map + size_t(b) * size_t(s.stride) * size_t(s.h), s.stride, {0, 0, s.w, s.h});
                s.uploaded = b;
            }
            draws.push_back({s.tex, Rect::from(l["dst"]), Rect::from(l["clip"])});
        }
        glFinish();  // the core may hand the buffers back to their plugins now
        send_seq("uploaded", seq);

        glViewport(0, 0, w_, h_);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(prog_);
        glEnable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        glUniform1f(u_opaque_, 1.f);
        for (const auto& d : draws) {
            scissor(d.clip);
            quad(d.tex, d.dst);
        }
        glScissor(0, 0, w_, h_);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glUniform1f(u_opaque_, 0.f);
        quad(canvas_tex_, {0, 0, canvas_tex_.w, canvas_tex_.h});
        present(seq);
    }

    // A frame from drawing commands: new ones are turned into quads (the
    // masks and images they bring uploaded); without new ones, the last
    // frame's quads are drawn again with the surfaces' new contents.
    void ops_frame(const Json& msg) {
        int seq = msg["seq"].as_int();
        layer_tex_.clear();
        layer_dst_.clear();
        layer_native_.clear();
        for (const auto& l : msg["layers"].items()) {
            Texture t;
            auto it = surfaces_.find(l["key"].str());
            int b = l["buffer"].as_int(-1), slot = l["gpu"].as_int(-1);
            bool native = false;
            if (it != surfaces_.end() && slot >= 0 && slot < 8) {
                t.id = it->second.gpu[slot].tex;  // drawn from the plugin's GPU buffer, no upload
                native = true;
            } else if (it != surfaces_.end() && b >= 0 && b < it->second.buffers) {
                Surface& s = it->second;
                if (b != s.uploaded) {
                    upload(s.tex, s.map + size_t(b) * size_t(s.stride) * size_t(s.h), s.stride, {0, 0, s.w, s.h});
                    s.uploaded = b;
                }
                t = s.tex;
            }
            layer_tex_.push_back(t);
            layer_dst_.push_back(Rect::from(l["dst"]));
            layer_native_.push_back(native);
        }
        if (msg.contains("ops")) parse_ops(size_t(msg["ops"].as_number()));
        glFinish();  // the core may reuse the command memory and the surface buffers now
        send_seq("uploaded", seq);

        glViewport(0, 0, w_, h_);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(ops_prog_);
        glUniform2f(u_size_, float(w_), float(h_));
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glActiveTexture(GL_TEXTURE0);
        for (GLint a : {o_pos_, o_uv_, o_edge_, o_clip_, o_color_, o_param_})
            if (a >= 0) glEnableVertexAttribArray(GLuint(a));
        for (const auto& b : batches_) {
            glBindTexture(GL_TEXTURE_2D, texture_of(b.tex));  // 0 when a surface is gone: black
            bool native = b.tex.kind == TexRef::Layer && size_t(b.tex.index) < layer_native_.size() &&
                          layer_native_[size_t(b.tex.index)];
            glUniform1f(u_native_, native ? 1.f : 0.f);
            const Vertex* v = verts_.data() + b.first;
            glVertexAttribPointer(GLuint(o_pos_), 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), &v->x);
            glVertexAttribPointer(GLuint(o_uv_), 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), &v->u);
            glVertexAttribPointer(GLuint(o_edge_), 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), v->edge);
            glVertexAttribPointer(GLuint(o_clip_), 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), v->clip);
            glVertexAttribPointer(GLuint(o_color_), 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), v->color);
            glVertexAttribPointer(GLuint(o_param_), 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), &v->radius);
            glDrawElements(GL_TRIANGLES, GLsizei(b.quads * 6), GL_UNSIGNED_SHORT, quad_indices_.data());
        }
        present(seq);
    }

    void power(bool on) {
        if (drm_ >= 0 && dpms_prop_) drmModeConnectorSetProperty(drm_, conn_, dpms_prop_, on ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF);
    }

    // Page-flip completions from DRM.
    void drm_event() {
        drmEventContext ev{};
        ev.version = 2;
        ev.page_flip_handler = [](int, unsigned, unsigned, unsigned, void* data) {
            static_cast<Gpu*>(data)->flipped();
        };
        drmHandleEvent(drm_, &ev);
    }

    void set_dump(const char* path) { dump_ = path ? path : ""; }

private:
    static constexpr size_t kMaxBatchQuads = 16000;  // 16-bit indices

    GLuint texture_of(const TexRef& r) const {
        switch (r.kind) {
            case TexRef::Page: return size_t(r.index) < pages_.size() ? pages_[size_t(r.index)].id : 0;
            case TexRef::Inline: return size_t(r.index) < inline_.size() ? inline_[size_t(r.index)].id : 0;
            case TexRef::Image: return size_t(r.index) < images_.size() ? images_[size_t(r.index)].id : 0;
            case TexRef::Layer: return size_t(r.index) < layer_tex_.size() ? layer_tex_[size_t(r.index)].id : 0;
            default: return 0;
        }
    }

    // A texture of `slot` in `pool`, (re)made for w x h.
    static Texture& pooled(std::vector<Texture>& pool, size_t slot, int w, int h, GLenum format) {
        if (pool.size() <= slot) pool.resize(slot + 1);
        Texture& t = pool[slot];
        if (t.id && (t.w != w || t.h != h)) {
            glDeleteTextures(1, &t.id);
            t.id = 0;
        }
        if (!t.id) {
            t.w = w, t.h = h;
            glGenTextures(1, &t.id);
            glBindTexture(GL_TEXTURE_2D, t.id);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GLint(format), w, h, 0, format, GL_UNSIGNED_BYTE, nullptr);
        }
        return t;
    }

    void alpha_upload(GLuint tex, int x, int y, int w, int h, const uint8_t* px) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_ALPHA, GL_UNSIGNED_BYTE, px);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    }

    struct Box {
        float x0, y0, x1, y1;
    };

    void quad(const Box& g, float u0, float v0, float u1, float v1, const Box& edge, uint32_t color, float radius,
              float mode, TexRef tex) {
        // Shapes do not sample: they join any batch. Masks and images need
        // their texture bound, so a batch takes them while it has none or the same.
        bool need = mode > 0.5f;
        Batch* b = batches_.empty() ? nullptr : &batches_.back();
        if (b && b->quads < kMaxBatchQuads && need && !(b->tex == tex)) {
            if (b->tex.kind == TexRef::None) b->tex = tex;
            else b = nullptr;
        }
        if (!b || b->quads >= kMaxBatchQuads) {
            batches_.push_back({need ? tex : TexRef{}, verts_.size(), 0});
            b = &batches_.back();
        }
        const float px[4] = {g.x0, g.x1, g.x0, g.x1}, py[4] = {g.y0, g.y0, g.y1, g.y1};
        const float pu[4] = {u0, u1, u0, u1}, pv[4] = {v0, v0, v1, v1};
        for (int i = 0; i < 4; ++i) {
            Vertex v;
            v.x = px[i], v.y = py[i], v.u = pu[i], v.v = pv[i];
            v.edge[0] = px[i] - edge.x0, v.edge[1] = py[i] - edge.y0;
            v.edge[2] = edge.x1 - px[i], v.edge[3] = edge.y1 - py[i];
            v.clip[0] = px[i] - clip_.x0, v.clip[1] = py[i] - clip_.y0;
            v.clip[2] = clip_.x1 - px[i], v.clip[3] = clip_.y1 - py[i];
            std::memcpy(v.color, &color, 4);
            v.radius = radius, v.mode = mode;
            verts_.push_back(v);
        }
        ++b->quads;
    }

    // Reads the commands into quads; uploads masks and images on the way.
    void parse_ops(size_t bytes) {
        verts_.clear();
        batches_.clear();
        clip_ = {0, 0, float(w_), float(h_)};
        size_t inline_used = 0, images_used = 0;
        const Box none{-1e4f, -1e4f, 1e4f, 1e4f};  // no shape edges (masks and images)
        if (!ops_map_ || bytes > ops_size_) return;
        size_t at = 0;
        while (at + sizeof(ops::Header) <= bytes) {
            const auto* h = reinterpret_cast<const ops::Header*>(ops_map_ + at);
            if (h->size < sizeof(ops::Header) || at + h->size > bytes || (h->size & 3)) break;
            const uint8_t* rec = ops_map_ + at;
            size_t payload = h->size;
            at += h->size;
            switch (h->type) {
                case ops::kClip: {
                    if (payload < sizeof(ops::Clip)) break;
                    const auto* o = reinterpret_cast<const ops::Clip*>(rec);
                    clip_ = {o->x, o->y, o->x + o->w, o->y + o->h_};
                    break;
                }
                case ops::kRect: {
                    if (payload < sizeof(ops::Rect)) break;
                    const auto* o = reinterpret_cast<const ops::Rect*>(rec);
                    Box shape{o->x, o->y, o->x + o->w, o->y + o->h_};
                    // One pixel around the shape for its soft edge, never past the clip.
                    Box g{std::max(shape.x0 - 1, clip_.x0 - 1), std::max(shape.y0 - 1, clip_.y0 - 1),
                          std::min(shape.x1 + 1, clip_.x1 + 1), std::min(shape.y1 + 1, clip_.y1 + 1)};
                    if (g.x1 <= g.x0 || g.y1 <= g.y0) break;
                    quad(g, 0, 0, 0, 0, shape, o->color, o->radius, 0, {});
                    break;
                }
                case ops::kUpload: {
                    if (payload < sizeof(ops::Upload)) break;
                    const auto* o = reinterpret_cast<const ops::Upload*>(rec);
                    if (o->page >= uint32_t(ops::kMaxAtlasPages) || o->w <= 0 || o->h_ <= 0 ||
                        payload < sizeof(ops::Upload) + size_t(o->w) * size_t(o->h_))
                        break;
                    Texture& page = pooled(pages_, o->page, ops::kAtlasSize, ops::kAtlasSize, GL_ALPHA);
                    alpha_upload(page.id, o->u, o->v, o->w, o->h_, reinterpret_cast<const uint8_t*>(o + 1));
                    break;
                }
                case ops::kMask: {
                    if (payload < sizeof(ops::Mask)) break;
                    const auto* o = reinterpret_cast<const ops::Mask*>(rec);
                    if (o->w <= 0 || o->h_ <= 0) break;
                    Box g{float(o->x), float(o->y), float(o->x + o->w), float(o->y + o->h_)};
                    if (o->page == ops::kInlinePage) {
                        if (payload < sizeof(ops::Mask) + size_t(o->w) * size_t(o->h_)) break;
                        size_t slot = inline_used++;
                        Texture& t = pooled(inline_, slot, o->w, o->h_, GL_ALPHA);
                        alpha_upload(t.id, 0, 0, o->w, o->h_, reinterpret_cast<const uint8_t*>(o + 1));
                        quad(g, 0, 0, 1, 1, none, o->color, 0, 1, {TexRef::Inline, int(slot)});
                    } else if (o->page < uint32_t(ops::kMaxAtlasPages)) {
                        const float s = 1.f / float(ops::kAtlasSize);
                        quad(g, float(o->u) * s, float(o->v) * s, float(o->u + o->w) * s, float(o->v + o->h_) * s, none,
                             o->color, 0, 1, {TexRef::Page, int(o->page)});
                    }
                    break;
                }
                case ops::kSurface: {
                    if (payload < sizeof(ops::Surface)) break;
                    const auto* o = reinterpret_cast<const ops::Surface*>(rec);
                    if (o->layer >= layer_dst_.size()) break;
                    const Rect& d = layer_dst_[o->layer];
                    Box g{float(d.x), float(d.y), float(d.x + d.w), float(d.y + d.h)};
                    quad(g, 0, 0, 1, 1, none, 0xFFFFFFFFu, 0, 2, {TexRef::Layer, int(o->layer)});
                    break;
                }
                case ops::kImage: {
                    if (payload < sizeof(ops::Image)) break;
                    const auto* o = reinterpret_cast<const ops::Image*>(rec);
                    if (o->w <= 0 || o->h_ <= 0 || payload < sizeof(ops::Image) + size_t(o->w) * size_t(o->h_) * 4) break;
                    size_t slot = images_used++;
                    Texture& t = pooled(images_, slot, o->w, o->h_, GL_RGBA);
                    upload(t, reinterpret_cast<const uint8_t*>(o + 1), o->w * 4, {0, 0, o->w, o->h_});
                    Box g{o->x, o->y, o->x + o->dw, o->y + o->dh};
                    quad(g, 0, 0, 1, 1, none, 0xFFFFFFFFu, 0, 2, {TexRef::Image, int(slot)});
                    break;
                }
                default: break;  // kAtlasReset and unknown records: nothing to draw
            }
        }
        // Textures of masks and images no longer used are freed.
        auto trim = [](std::vector<Texture>& pool, size_t used) {
            for (size_t i = used; i < pool.size(); ++i)
                if (pool[i].id) glDeleteTextures(1, &pool[i].id);
            pool.resize(std::min(pool.size(), used));
        };
        trim(inline_, inline_used);
        trim(images_, images_used);
    }

    bool fail(std::string& error, const std::string& e) {
        error = e;
        return false;
    }

    // `window`: draws into the GBM surface (the screen); otherwise into an FBO
    // without any surface, with GBM's configurations when `gbm`.
    bool init_egl(bool window, std::string& error, bool gbm = false) {
        EGLint major = 0, minor = 0;
        if (dpy_ == EGL_NO_DISPLAY || !eglInitialize(dpy_, &major, &minor)) return fail(error, "EGL is unavailable");
        eglBindAPI(EGL_OPENGL_ES_API);
        EGLint attrs[] = {EGL_SURFACE_TYPE, window || gbm ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
                          EGL_BLUE_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
        EGLConfig configs[64];
        EGLint n = 0;
        eglChooseConfig(dpy_, attrs, configs, 64, &n);
        EGLConfig config = nullptr;
        for (EGLint i = 0; i < n && !config; ++i) {
            EGLint id = 0;
            eglGetConfigAttrib(dpy_, configs[i], EGL_NATIVE_VISUAL_ID, &id);
            if (!window || id == GBM_FORMAT_XRGB8888) config = configs[i];
        }
        if (!config && n > 0 && !window) config = configs[0];
        if (!config) return fail(error, "no suitable EGL configuration");
        // ES 3 for sub-rectangle uploads; ES 2 works too (whole rows).
        for (int version : {3, 2}) {
            EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, version, EGL_NONE};
            ctx_ = eglCreateContext(dpy_, config, EGL_NO_CONTEXT, ctx_attrs);
            if (ctx_ != EGL_NO_CONTEXT) {
                gles3_ = version == 3;
                break;
            }
        }
        if (ctx_ == EGL_NO_CONTEXT) return fail(error, "cannot create an OpenGL ES context");
        if (window) {
            surf_ = eglCreateWindowSurface(dpy_, config, EGLNativeWindowType(gsurf_), nullptr);
            if (surf_ == EGL_NO_SURFACE) return fail(error, "cannot create the EGL window surface");
        }
        if (!eglMakeCurrent(dpy_, surf_, surf_, ctx_)) return fail(error, "cannot make the context current");
        prog_ = program(kVertex, kFragment);
        ops_prog_ = program(kOpsVertex, kOpsFragment);
        if (!prog_ || !ops_prog_) return fail(error, "cannot build the shaders");
        o_pos_ = glGetAttribLocation(ops_prog_, "pos");
        o_uv_ = glGetAttribLocation(ops_prog_, "uv");
        o_edge_ = glGetAttribLocation(ops_prog_, "edge");
        o_clip_ = glGetAttribLocation(ops_prog_, "clip");
        o_color_ = glGetAttribLocation(ops_prog_, "color");
        o_param_ = glGetAttribLocation(ops_prog_, "param");
        u_size_ = glGetUniformLocation(ops_prog_, "size");
        u_native_ = glGetUniformLocation(ops_prog_, "native");
        // GPU buffers of plugins: imported as EGL images.
        const char* egl_ext = eglQueryString(dpy_, EGL_EXTENSIONS);
        const char* gl_ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        create_image_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        image_target_ =
            reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        dmabuf_ = egl_ext && gl_ext && std::strstr(egl_ext, "EGL_EXT_image_dma_buf_import") &&
                  std::strstr(gl_ext, "GL_OES_EGL_image") && create_image_ && destroy_image_ && image_target_;
        dmabuf_modifiers_ = dmabuf_ && std::strstr(egl_ext, "EGL_EXT_image_dma_buf_import_modifiers");
        glUseProgram(ops_prog_);
        glUniform1i(glGetUniformLocation(ops_prog_, "tex"), 0);
        quad_indices_.resize(kMaxBatchQuads * 6);
        for (size_t q = 0; q < kMaxBatchQuads; ++q) {
            GLushort b = GLushort(q * 4);
            const GLushort idx[6] = {b, GLushort(b + 1), GLushort(b + 2), GLushort(b + 2), GLushort(b + 1), GLushort(b + 3)};
            std::copy(idx, idx + 6, quad_indices_.begin() + long(q * 6));
        }
        a_pos_ = glGetAttribLocation(prog_, "pos");
        a_uv_ = glGetAttribLocation(prog_, "uv");
        u_opaque_ = glGetUniformLocation(prog_, "opaque");
        glUseProgram(prog_);
        glUniform1i(glGetUniformLocation(prog_, "tex"), 0);
        return true;
    }

    static GLuint shader(GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, sizeof log, nullptr, log);
            std::fprintf(stderr, "facet-gpu: shader: %s\n", log);
            return 0;
        }
        return s;
    }

    static GLuint program(const char* vertex, const char* fragment) {
        GLuint vs = shader(GL_VERTEX_SHADER, vertex), fs = shader(GL_FRAGMENT_SHADER, fragment);
        if (!vs || !fs) return 0;
        GLuint p = glCreateProgram();
        glAttachShader(p, vs);
        glAttachShader(p, fs);
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        return ok ? p : 0;
    }

    static Texture make_texture(int w, int h, bool smooth) {
        Texture t{0, w, h};
        glGenTextures(1, &t.id);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        return t;
    }

    // Copies a rectangle of a pixel buffer (`stride` bytes per row) into the texture.
    void upload(const Texture& t, const uint8_t* px, int stride, Rect r) {
        if (!px || !t.id) return;
        r.x = std::max(0, r.x), r.y = std::max(0, r.y);
        r.w = std::min(r.w, t.w - r.x), r.h = std::min(r.h, t.h - r.y);
        if (r.w <= 0 || r.h <= 0) return;
        glBindTexture(GL_TEXTURE_2D, t.id);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        if (!gles3_ || stride != t.w * 4) {
            // Whole rows (ES 2 has no row length); the stride must match.
            if (stride != t.w * 4) return;
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, r.y, t.w, r.h, GL_RGBA, GL_UNSIGNED_BYTE,
                            px + size_t(r.y) * size_t(stride));
            return;
        }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, t.w);
        glTexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y, r.w, r.h, GL_RGBA, GL_UNSIGNED_BYTE,
                        px + size_t(r.y) * size_t(stride) + size_t(r.x) * 4);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    void scissor(const Rect& c) { glScissor(c.x, h_ - (c.y + c.h), std::max(0, c.w), std::max(0, c.h)); }

    void quad(const Texture& t, const Rect& d) {
        if (!t.id) return;
        // Attribute arrays are context state shared by both programs: only
        // these two may be on here, or the GPU reads arrays nobody set.
        for (GLint a : {o_pos_, o_uv_, o_edge_, o_clip_, o_color_, o_param_})
            if (a >= 0 && a != a_pos_ && a != a_uv_) glDisableVertexAttribArray(GLuint(a));
        float x0 = float(d.x) / float(w_) * 2.f - 1.f, x1 = float(d.x + d.w) / float(w_) * 2.f - 1.f;
        float y0 = 1.f - float(d.y) / float(h_) * 2.f, y1 = 1.f - float(d.y + d.h) / float(h_) * 2.f;
        const GLfloat pos[] = {x0, y0, x1, y0, x0, y1, x1, y1};
        const GLfloat uv[] = {0, 0, 1, 0, 0, 1, 1, 1};
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, t.id);
        glVertexAttribPointer(GLuint(a_pos_), 2, GL_FLOAT, GL_FALSE, 0, pos);
        glVertexAttribPointer(GLuint(a_uv_), 2, GL_FLOAT, GL_FALSE, 0, uv);
        glEnableVertexAttribArray(GLuint(a_pos_));
        glEnableVertexAttribArray(GLuint(a_uv_));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    struct Fb {
        int drm;
        uint32_t id;
    };

    uint32_t fb_for(gbm_bo* bo) {
        if (auto* fb = static_cast<Fb*>(gbm_bo_get_user_data(bo))) return fb->id;
        uint32_t handles[4] = {gbm_bo_get_handle(bo).u32}, pitches[4] = {gbm_bo_get_stride(bo)}, offsets[4] = {0};
        uint32_t id = 0;
        if (drmModeAddFB2(drm_, gbm_bo_get_width(bo), gbm_bo_get_height(bo), GBM_FORMAT_XRGB8888, handles, pitches,
                          offsets, &id, 0) != 0)
            return 0;
        gbm_bo_set_user_data(bo, new Fb{drm_, id}, [](gbm_bo*, void* data) {
            auto* fb = static_cast<Fb*>(data);
            drmModeRmFB(fb->drm, fb->id);
            delete fb;
        });
        return id;
    }

    void present(int seq) {
        if (fbo_) {  // offscreen: the frame is "shown" at once
            if (!dump_.empty()) dump();
            send_seq("shown", seq);
            return;
        }
        eglSwapBuffers(dpy_, surf_);
        gbm_bo* bo = gbm_surface_lock_front_buffer(gsurf_);
        uint32_t fb = bo ? fb_for(bo) : 0;
        if (!fb) {
            if (bo) gbm_surface_release_buffer(gsurf_, bo);
            send_seq("shown", seq);
            return;
        }
        pending_seq_ = seq;
        if (!mode_set_) {
            drmModeSetCrtc(drm_, crtc_, fb, 0, 0, &conn_, 1, &mode_);
            mode_set_ = true;
            next_bo_ = bo;
            flipped();
            return;
        }
        next_bo_ = bo;
        if (drmModePageFlip(drm_, crtc_, fb, DRM_MODE_PAGE_FLIP_EVENT, this) != 0) {
            drmModeSetCrtc(drm_, crtc_, fb, 0, 0, &conn_, 1, &mode_);  // no flip (e.g. display off): set directly
            flipped();
        }
    }

    void flipped() {
        if (cur_bo_) gbm_surface_release_buffer(gsurf_, cur_bo_);
        cur_bo_ = next_bo_;
        next_bo_ = nullptr;
        send_seq("shown", pending_seq_);
    }

    void send_seq(const char* t, int seq) {
        Json m = Json::object();
        m["t"] = t;
        m["seq"] = seq;
        ch_.send(m);
    }

    void dump() {
        std::vector<uint8_t> px(size_t(w_) * size_t(h_) * 4);
        glReadPixels(0, 0, w_, h_, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        FILE* f = std::fopen(dump_.c_str(), "wb");
        if (!f) return;
        std::fprintf(f, "P6\n%d %d\n255\n", w_, h_);
        for (int y = h_ - 1; y >= 0; --y)
            for (int x = 0; x < w_; ++x) std::fwrite(&px[(size_t(y) * size_t(w_) + size_t(x)) * 4], 1, 3, f);
        std::fclose(f);
    }

    void shutdown() {
        if (drm_ >= 0 && saved_) {  // give the screen back as it was (console)
            drmModeSetCrtc(drm_, saved_->crtc_id, saved_->buffer_id, saved_->x, saved_->y, &conn_, 1, &saved_->mode);
            drmModeFreeCrtc(saved_);
            saved_ = nullptr;
        }
        if (dpy_ != EGL_NO_DISPLAY) {
            eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            eglTerminate(dpy_);
            dpy_ = EGL_NO_DISPLAY;
        }
        if (gsurf_) gbm_surface_destroy(gsurf_);
        if (gbm_) gbm_device_destroy(gbm_);
        gsurf_ = nullptr;
        gbm_ = nullptr;
        if (drm_ >= 0) {
            drmDropMaster(drm_);
            ::close(drm_);
            drm_ = -1;
        }
        if (render_fd_ >= 0) ::close(render_fd_);
        render_fd_ = -1;
    }

    Channel& ch_;
    int drm_ = -1, w_ = 0, h_ = 0;
    uint32_t conn_ = 0, crtc_ = 0, dpms_prop_ = 0;
    drmModeModeInfo mode_{};
    drmModeCrtc* saved_ = nullptr;
    gbm_device* gbm_ = nullptr;
    gbm_surface* gsurf_ = nullptr;
    gbm_bo *cur_bo_ = nullptr, *next_bo_ = nullptr;
    bool mode_set_ = false, gles3_ = false;
    int pending_seq_ = 0;
    EGLDisplay dpy_ = EGL_NO_DISPLAY;
    EGLContext ctx_ = EGL_NO_CONTEXT;
    EGLSurface surf_ = EGL_NO_SURFACE;
    GLuint prog_ = 0, fbo_ = 0, rbo_ = 0, ops_prog_ = 0;
    GLint a_pos_ = -1, a_uv_ = -1, u_opaque_ = -1;
    GLint o_pos_ = -1, o_uv_ = -1, o_edge_ = -1, o_clip_ = -1, o_color_ = -1, o_param_ = -1, u_size_ = -1,
          u_native_ = -1;
    PFNEGLCREATEIMAGEKHRPROC create_image_ = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image_ = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_ = nullptr;
    bool dmabuf_ = false, dmabuf_modifiers_ = false;
    int render_fd_ = -1;
    std::vector<bool> layer_native_;
    const uint8_t* ops_map_ = nullptr;
    size_t ops_size_ = 0;
    std::vector<Vertex> verts_;
    std::vector<Batch> batches_;
    std::vector<GLushort> quad_indices_;
    Box clip_{};
    std::vector<Texture> pages_, inline_, images_, layer_tex_;
    std::vector<Rect> layer_dst_;
    Texture canvas_tex_;
    const uint8_t* canvas_map_ = nullptr;
    size_t canvas_size_ = 0;
    std::map<std::string, Surface> surfaces_;
    std::string dump_;
};

}  // namespace

int main(int argc, char** argv) {
    int fd = -1, ow = 0, oh = 0;
    const char *card = nullptr, *dump = nullptr, *device = nullptr;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--fd") fd = std::atoi(argv[i + 1]);
        else if (a == "--card") card = argv[i + 1];
        else if (a == "--offscreen") std::sscanf(argv[i + 1], "%dx%d", &ow, &oh);
        else if (a == "--dump") dump = argv[i + 1];
        else if (a == "--device") device = argv[i + 1];
    }
    if (fd < 0 || (!card && ow <= 0)) {
        std::fprintf(stderr,
                     "usage: facet-gpu --fd N (--card /dev/dri/cardN | --offscreen WxH [--device /dev/dri/renderDN]"
                     " [--dump file])\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa{};
    sa.sa_handler = [](int) { g_quit = 1; };
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    Channel ch;
    ch.adopt(fd, false);
    Gpu gpu(ch);
    gpu.set_dump(dump);
    std::string error;
    bool ok = card ? gpu.init_kms(card, error) : gpu.init_offscreen(ow, oh, device, error);
    Json hello = Json::object();
    if (!ok) {
        hello["t"] = "error";
        hello["message"] = error;
        ch.send(hello);
        std::fprintf(stderr, "facet-gpu: %s\n", error.c_str());
        return 1;
    }
    hello["t"] = "ready";
    hello["w"] = gpu.width();
    hello["h"] = gpu.height();
    hello["renderer"] = gpu.renderer();
    hello["ops"] = true;  // draws the interface from commands (gfx/ops.h)
    hello["dmabuf"] = gpu.dmabuf_supported();  // shows plugins' GPU buffers
    ch.send(hello);

    while (!g_quit) {
        pollfd fds[2] = {{ch.fd(), POLLIN, 0}, {gpu.drm_fd(), POLLIN, 0}};
        int n = ::poll(fds, gpu.drm_fd() >= 0 ? 2 : 1, 1000);
        if (n < 0) continue;  // EINTR: g_quit is checked above
        if (gpu.drm_fd() >= 0 && (fds[1].revents & POLLIN)) gpu.drm_event();
        if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        std::vector<std::pair<Json, int>> msgs;
        bool alive = ch.read(msgs);
        for (size_t i = 0; i < msgs.size(); ++i) {
            const Json& m = msgs[i].first;
            int mfd = msgs[i].second;
            const std::string& t = m["t"].str();
            if (t == "canvas" && mfd >= 0) gpu.set_canvas(mfd, m["w"].as_int(), m["h"].as_int());
            else if (t == "ops" && mfd >= 0) gpu.set_ops(mfd, size_t(m["size"].as_number()));
            else if (t == "surface" && mfd >= 0)
                gpu.add_surface(m["key"].str(), mfd, m["w"].as_int(), m["h"].as_int(), m["stride"].as_int(),
                                m["buffers"].as_int());
            else if (t == "surface_drop") gpu.drop_surface(m["key"].str());
            else if (t == "surface_gpu" && mfd >= 0) gpu.add_gpu_buffer(m, mfd);
            else if (t == "frame") gpu.frame(m);
            else if (t == "power") gpu.power(m["on"].as_bool(true));
            else if (t == "quit") g_quit = 1;
            else if (mfd >= 0) ::close(mfd);
        }
        if (!alive) break;  // the core is gone: give the screen back
    }
    return 0;
}
