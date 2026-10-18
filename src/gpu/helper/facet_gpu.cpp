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
#include <cmath>
#include <cstddef>

#include <deque>
#include <ctime>
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
#include <unordered_map>
#include <vector>

#include "gfx/ops.h"
#include "gpu/channel.h"

namespace ops = facet::gfx::ops;

using facet::Json;
using facet::gpu::Channel;

#ifndef EGL_BUFFER_AGE_EXT
#define EGL_BUFFER_AGE_EXT 0x313D
#endif

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
uniform vec2 origin;  // the canvas point at the target's top left (a kept layer starts elsewhere)
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
    vec2 p = pos - origin;
    gl_Position = vec4(p.x / size.x * 2.0 - 1.0, 1.0 - p.y / size.y * 2.0, 0.0, 1.0);
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
    if (v_param.y > 4.5) {  // an ellipse inside the shape's box
        vec2 p = vec2(v_edge.x - v_edge.z, v_edge.y - v_edge.w) * 0.5;
        vec2 r = max(vec2(v_edge.x + v_edge.z, v_edge.y + v_edge.w) * 0.5, vec2(0.01));
        float k0 = length(p / r);
        float k1 = max(length(p / (r * r)), 0.000001);
        cov *= clamp(0.5 - (k0 - 1.0) * k0 / k1, 0.0, 1.0);  // distance to the outline, about
    } else if (v_param.y > 3.5) {  // a kept layer: its colours are premultiplied already
        gl_FragColor = texture2D(tex, v_uv) * cov;
        return;
    }
    else if (v_param.y > 2.5) {  // light around a shape: strongest inside, fading with the distance
        float r = v_param.x;
        vec2 q = vec2(r) - min(v_edge.xy, v_edge.zw);
        float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
        cov *= pow(clamp(1.0 - max(d, 0.0) / v_uv.x, 0.0, 1.0), 2.2);
    } else if (v_param.y < 0.5) {
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
    float radius, mode;  // mode: 0 shape, 1 alpha mask, 2 image, 3 glow, 4 kept layer, 5 ellipse
};

// What a batch of quads samples.
struct TexRef {
    enum Kind { None, Page, Inline, Image, Layer } kind = None;
    int index = 0;
    bool operator==(const TexRef& o) const { return kind == o.kind && index == o.index; }
};

struct Batch {
    // Draw: quads from `first`. The others carry no quads: the drawing of a kept
    // layer starts (Begin) and ends (End), and its texture is laid over the picture (Blit).
    enum Kind { Draw, Begin, End, Blit } kind = Draw;
    TexRef tex;
    size_t first = 0;  // vertex
    size_t quads = 0;
    uint32_t layer = 0;
    uint32_t target = 0;  // Draw: the kept layer it draws into, 0 for the screen
    float x = 0, y = 0, w = 0, h = 0;  // Begin, Blit: where the layer is on the canvas
    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;  // Draw: where its quads are
};

// A layer the core asked to keep (ops::Layer): drawn into `tex` through `fbo`.
struct KeptLayer {
    Texture tex;
    GLuint fbo = 0;
};

// What one quad of a frame draws, for finding what changed since the last
// frame: equal signatures draw the same pixels.
struct QInfo {
    float x0, y0, x1, y1;  // where it draws (canvas pixels)
    uint64_t sig;
    bool vol = false;      // a plugin's surface: its pixels may have changed
    int page = -1;         // a mask of this atlas page, sampling u0..u1, v0..v1
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};

// What of a target (the screen or a kept layer) is to be drawn again, in tiles.
struct Tiles {
    static constexpr int kTile = 64;
    int w = 0, h = 0, tw = 0, th = 0;
    bool full = true;
    std::vector<uint8_t> bits;

    void reset(int w_, int h_) {
        w = w_, h = h_;
        tw = (w + kTile - 1) / kTile, th = (h + kTile - 1) / kTile;
        full = false;
        bits.assign(size_t(tw) * size_t(th), 0);
    }
    void mark(float x0, float y0, float x1, float y1) {
        if (full) return;
        int a = std::max(0, int(std::floor(x0)) / kTile), b = std::max(0, int(std::floor(y0)) / kTile);
        int c = std::min(tw - 1, int(std::ceil(x1)) / kTile), d = std::min(th - 1, int(std::ceil(y1)) / kTile);
        if (x1 <= 0 || y1 <= 0 || x0 >= float(w) || y0 >= float(h)) return;
        for (int ty = b; ty <= d; ++ty)
            for (int tx = a; tx <= c; ++tx) bits[size_t(ty) * size_t(tw) + size_t(tx)] = 1;
    }
    void merge(const Tiles& o) {
        if (full) return;
        if (o.full || o.bits.size() != bits.size()) {
            full = true;
            return;
        }
        for (size_t i = 0; i < bits.size(); ++i) bits[i] |= o.bits[i];
    }
    // Rectangles (pixels) covering the dirty tiles: runs of a row joined with the same run above.
    std::vector<Rect> rects(size_t max) const {
        std::vector<Rect> out;
        struct Run {
            int a, b, y0;
        };
        std::vector<Run> open;
        auto flush = [&](const Run& r, int y1) {
            Rect q{r.a * kTile, r.y0 * kTile, std::min(w, r.b * kTile) - r.a * kTile, std::min(h, y1 * kTile) - r.y0 * kTile};
            out.push_back(q);
        };
        for (int ty = 0; ty <= th; ++ty) {
            std::vector<Run> row;
            for (int tx = 0; ty < th && tx < tw;) {
                if (!bits[size_t(ty) * size_t(tw) + size_t(tx)]) {
                    ++tx;
                    continue;
                }
                int a = tx;
                while (tx < tw && bits[size_t(ty) * size_t(tw) + size_t(tx)]) ++tx;
                row.push_back({a, tx, ty});
            }
            std::vector<Run> next;
            for (auto& r : row) {
                bool joined = false;
                for (auto& o : open)
                    if (o.a == r.a && o.b == r.b) {
                        r.y0 = o.y0;
                        o.a = -1;  // taken
                        joined = true;
                        break;
                    }
                (void)joined;
                next.push_back(r);
            }
            for (auto& o : open)
                if (o.a >= 0) flush(o, ty);
            open = next;
        }
        if (out.size() > max) {
            int x0 = w, y0 = h, x1 = 0, y1 = 0;
            for (auto& r : out) {
                x0 = std::min(x0, r.x), y0 = std::min(y0, r.y);
                x1 = std::max(x1, r.x + r.w), y1 = std::max(y1, r.y + r.h);
            }
            out = {{x0, y0, x1 - x0, y1 - y0}};
        }
        return out;
    }
};

// The quads of one target, this frame and the last one drawn.
struct TargetList {
    std::vector<QInfo> cur, prev;
    bool have_prev = false;
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
        force_full_ = true;
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
        force_full_ = true;
        hist_.clear();
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
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
    // frame's quads are drawn again with the surfaces' new contents. Only
    // what differs from the last frame is drawn again: the screen keeps its
    // pixels (the buffers of a window surface know how old they are).
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
        for (auto& kv : lists_)
            for (auto& q : kv.second.cur)
                if (q.vol) q.sig = ++unique_;  // a surface may show something new
        if (stats_) report_stats();
        glFinish();  // the core may reuse the command memory and the surface buffers now
        send_seq("uploaded", seq);

        double gpu_t0 = 0;
        if (stats_) {
            glFinish();
            gpu_t0 = mono_now();
        }
        glUseProgram(ops_prog_);
        glEnable(GL_BLEND);
        glEnable(GL_SCISSOR_TEST);
        glActiveTexture(GL_TEXTURE0);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
        if (msg.contains("ops")) glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(verts_.size() * sizeof(Vertex)), verts_.data(), GL_STREAM_DRAW);
        for (GLint a : {o_pos_, o_uv_, o_edge_, o_clip_, o_color_, o_param_})
            if (a >= 0) glEnableVertexAttribArray(GLuint(a));

        // Kept layers drawn this frame first: what changed in them is part of the screen's damage.
        Tiles from_layers;
        from_layers.reset(w_, h_);
        bool any_layer = false;
        for (const auto& b : batches_)
            if (b.kind == Batch::Begin) {
                draw_kept_layer(b, from_layers);
                any_layer = true;
            }

        // The screen.
        TargetList& main = lists_[0];
        Tiles dm;
        dm.reset(w_, h_);
        if (force_full_ || !main.have_prev) dm.full = true;
        else {
            diff_lists(main.prev, main.cur, 0, 0, dm);
            dm.merge(from_layers);
        }
        Tiles region = dm;
        int age = 1;
        if (surf_ != EGL_NO_SURFACE) {
            EGLint a = 0;
            age = age_ext_ && eglQuerySurface(dpy_, surf_, EGL_BUFFER_AGE_EXT, &a) ? a : 0;
        }
        if (age <= 0 || size_t(age - 1) > hist_.size()) region.full = true;
        else
            for (int k = 0; k < age - 1; ++k) region.merge(hist_[size_t(k)]);

        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);  // 0 on the screen
        glViewport(0, 0, w_, h_);
        glUniform2f(u_size_, float(w_), float(h_));
        glUniform2f(u_origin_, 0.f, 0.f);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        draw_target(0, region, w_, h_, 0.f, 0.f, first_quad_covers_screen() ? -1 : 1);
        if (verify_ && fbo_) verify_frame();
        hist_.push_front(dm);
        if (hist_.size() > 4) hist_.pop_back();
        main.prev = main.cur;
        main.have_prev = true;
        force_full_ = false;
        (void)any_layer;

        if (stats_) {
            glFinish();
            bool redrawn = any_layer && from_layers.full;
            static unsigned n = 0;
            double fraction = 1;
            size_t nrects = 0;
            if (!region.full) {
                auto rs = region.rects(kMaxParts);
                nrects = rs.size();
                double area = 0;
                for (auto& r : rs) area += double(r.w) * double(r.h);
                fraction = area / (double(w_) * double(h_));
            }
            gpu_ms_hit_ += (mono_now() - gpu_t0) * 1000;
            if (++n % 100 == 0) {
                std::fprintf(stderr, "gpu time: %.1f ms per frame, the last one redrew %.0f%% of the screen in %zu parts (age %d)\n",
                             gpu_ms_hit_ / 100, fraction * 100, nrects, age);
                gpu_ms_hit_ = 0;
            }
            (void)redrawn;
        }
        present(seq);
    }

    // FACET_GPU_VERIFY (offscreen): draws everything again from scratch and compares it
    // with what the partial drawing made, for the kept layers and for the screen.
    void verify_frame() {
        auto read = [&](GLuint fbo, int w, int h, std::vector<uint8_t>& out) {
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            out.resize(size_t(w) * size_t(h) * 4);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
        };
        auto count = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
            size_t bad = 0;
            for (size_t i = 0; i + 3 < a.size(); i += 4)
                if (std::memcmp(&a[i], &b[i], 4) != 0) ++bad;
            return bad;
        };
        auto scratch = [&](int w, int h, GLuint& fbo, GLuint& tex) {
            Texture t = make_texture(w, h, false);
            tex = t.id;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        };
        size_t bad_layers = 0, bad_screen = 0;
        std::vector<uint8_t> a, b;
        for (const auto& bt : batches_) {
            if (bt.kind != Batch::Begin) continue;
            auto it = kept_.find(bt.layer);
            if (it == kept_.end()) continue;
            const int w = it->second.tex.w, h = it->second.tex.h;
            GLuint fbo, tex;
            scratch(w, h, fbo, tex);
            glViewport(0, 0, w, h);
            glUniform2f(u_size_, float(w), float(h));
            glUniform2f(u_origin_, bt.x, bt.y);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
            Tiles all;
            all.full = true;
            draw_target(bt.layer, all, w, h, bt.x, bt.y, 0);
            read(fbo, w, h, a);
            read(it->second.fbo, w, h, b);
            bad_layers += count(a, b);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
        }
        {
            GLuint fbo, tex;
            scratch(w_, h_, fbo, tex);
            glViewport(0, 0, w_, h_);
            glUniform2f(u_size_, float(w_), float(h_));
            glUniform2f(u_origin_, 0.f, 0.f);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            Tiles all;
            all.full = true;
            draw_target(0, all, w_, h_, 0.f, 0.f, first_quad_covers_screen() ? -1 : 1);
            read(fbo, w_, h_, a);
            read(fbo_, w_, h_, b);
            bad_screen = count(a, b);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &tex);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glViewport(0, 0, w_, h_);
        ++verified_;
        if (bad_layers || bad_screen) {
            ++verify_bad_;
            std::fprintf(stderr, "verify: frame %u differs: %zu pixels of a kept layer, %zu of the screen\n", verified_, bad_layers,
                         bad_screen);
        }
        if (verified_ % 200 == 0)
            std::fprintf(stderr, "verify: %u frames checked, %u differed\n", verified_, verify_bad_);
    }

    // Whether the screen's first quad is opaque and covers all of it (then clearing is pointless).
    bool first_quad_covers_screen() const {
        for (const auto& b : batches_) {
            if (b.kind != Batch::Draw && b.kind != Batch::Blit) continue;
            if (b.kind != Batch::Draw || b.target != 0 || b.quads == 0) return false;
            const Vertex& v = verts_[b.first];
            if (v.mode != 0.f || v.radius >= 0.5f || v.color[3] != 255) return false;
            return v.x - v.edge[0] <= 0 && v.y - v.edge[1] <= 0 && v.x + v.edge[2] >= float(w_) &&
                   v.y + v.edge[3] >= float(h_) && v.x - v.clip[0] <= 0 && v.y - v.clip[1] <= 0 &&
                   v.x + v.clip[2] >= float(w_) && v.y + v.clip[3] >= float(h_);
        }
        return false;
    }

    // What differs between the last frame's quads and this frame's of a target (`ox`, `oy`:
    // where the target starts on the canvas, added to every position).
    void diff_lists(const std::vector<QInfo>& prev, const std::vector<QInfo>& cur, float ox, float oy, Tiles& d) {
        auto mark = [&](const QInfo& q) { d.mark(q.x0 + ox, q.y0 + oy, q.x1 + ox, q.y1 + oy); };
        if (prev.size() == cur.size()) {
            for (size_t i = 0; i < cur.size(); ++i)
                if (prev[i].sig != cur[i].sig) {
                    mark(prev[i]);
                    mark(cur[i]);
                }
        } else {
            size_t n = prev.size(), m = cur.size(), p = 0, sfx = 0;
            while (p < std::min(n, m) && prev[p].sig == cur[p].sig) ++p;
            while (sfx < std::min(n, m) - p && prev[n - 1 - sfx].sig == cur[m - 1 - sfx].sig) ++sfx;
            for (size_t i = p; i < n - sfx; ++i) mark(prev[i]);
            for (size_t i = p; i < m - sfx; ++i) mark(cur[i]);
        }
        // Masks of an atlas page that was written to may show something else now.
        for (const auto& q : cur) {
            if (q.page < 0) continue;
            for (const auto& u : uploads_) {
                if (u.page != q.page) continue;
                const float k = 1.f / float(ops::kAtlasSize);
                if (q.u0 < float(u.u + u.w) * k && q.u1 > float(u.u) * k && q.v0 < float(u.v + u.h) * k &&
                    q.v1 > float(u.v) * k) {
                    mark(q);
                    break;
                }
            }
        }
    }

    // Draws the dirty parts of a target: the screen (id 0, the current framebuffer) or a kept layer.
    // `ox`, `oy`: its place on the canvas; `clear`: 1 black, 0 transparent, -1 not at all.
    void draw_target(uint32_t id, const Tiles& dirty, int tw, int th, float ox, float oy, int clear) {
        std::vector<Rect> rects;
        if (dirty.full) rects = {{0, 0, tw, th}};
        else {
            rects = dirty.rects(kMaxParts);
            double area = 0;
            for (auto& r : rects) area += double(r.w) * double(r.h);
            if (area > 0.8 * double(tw) * double(th)) rects = {{0, 0, tw, th}};
        }
        for (const Rect& r : rects) {
            glScissor(r.x, th - (r.y + r.h), r.w, r.h);
            if (clear >= 0) {
                glClearColor(0, 0, 0, clear ? 1.f : 0.f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            const float rx0 = float(r.x) + ox, ry0 = float(r.y) + oy, rx1 = rx0 + float(r.w), ry1 = ry0 + float(r.h);
            for (const auto& b : batches_) {
                if (b.kind == Batch::Blit && id == 0) {
                    if (b.x < rx1 && b.x + b.w > rx0 && b.y < ry1 && b.y + b.h > ry0) blit_kept(b);
                    continue;
                }
                if (b.kind != Batch::Draw || b.target != id) continue;
                if (b.bx0 >= rx1 || b.bx1 <= rx0 || b.by0 >= ry1 || b.by1 <= ry0) continue;
                glBindTexture(GL_TEXTURE_2D, texture_of(b.tex));  // 0 when a surface is gone: black
                bool native = b.tex.kind == TexRef::Layer && size_t(b.tex.index) < layer_native_.size() &&
                              layer_native_[size_t(b.tex.index)];
                glUniform1f(u_native_, native ? 1.f : 0.f);
                set_attribs(b.first);
                glDrawElements(GL_TRIANGLES, GLsizei(b.quads * 6), GL_UNSIGNED_SHORT, nullptr);
            }
        }
    }

    // A kept layer that has to be drawn (again): into its texture, only where it changed. What
    // changed is added to `screen_damage` (canvas pixels).
    void draw_kept_layer(const Batch& b, Tiles& screen_damage) {
        const int w = std::max(1, int(std::ceil(b.w))), h = std::max(1, int(std::ceil(b.h)));
        KeptLayer& k = kept_[b.layer];
        bool fresh = false;
        if (k.tex.id && (k.tex.w != w || k.tex.h != h)) {
            glDeleteFramebuffers(1, &k.fbo);
            glDeleteTextures(1, &k.tex.id);
            k = KeptLayer{};
        }
        if (!k.tex.id) {
            k.tex = make_texture(w, h, false);
            glGenFramebuffers(1, &k.fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, k.fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, k.tex.id, 0);
            fresh = true;
        }
        TargetList& list = lists_[b.layer];
        Tiles d;
        d.reset(w, h);
        if (fresh || force_full_ || !list.have_prev) d.full = true;
        else diff_lists(list.prev, list.cur, -b.x, -b.y, d);
        if (stats_ && !d.full) {
            static unsigned nn = 0;
            if (++nn % 15 == 1) {
                size_t n = std::min(list.prev.size(), list.cur.size()), changed = 0;
                double big = 0;
                size_t bigi = 0;
                for (size_t i = 0; i < n; ++i)
                    if (list.prev[i].sig != list.cur[i].sig) {
                        ++changed;
                        double a = double(list.cur[i].x1 - list.cur[i].x0) * double(list.cur[i].y1 - list.cur[i].y0);
                        if (a > big) big = a, bigi = i;
                    }
                std::fprintf(stderr, "layer %u: %zu of %zu quads (was %zu) changed; the biggest #%zu is %.0fx%.0f at %.0f,%.0f\n", b.layer,
                             changed, list.cur.size(), list.prev.size(), bigi, bigi < n ? list.cur[bigi].x1 - list.cur[bigi].x0 : 0.f,
                             bigi < n ? list.cur[bigi].y1 - list.cur[bigi].y0 : 0.f, bigi < n ? list.cur[bigi].x0 : 0.f,
                             bigi < n ? list.cur[bigi].y0 : 0.f);
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, k.fbo);
        glViewport(0, 0, w, h);
        glUniform2f(u_size_, float(w), float(h));
        glUniform2f(u_origin_, b.x, b.y);
        // Colours end up premultiplied, as laid over the picture later.
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        draw_target(b.layer, d, w, h, b.x, b.y, 0);
        list.prev = list.cur;
        list.have_prev = true;
        if (d.full) screen_damage.mark(b.x, b.y, b.x + float(w), b.y + float(h));
        else
            for (const Rect& r : d.rects(kMaxParts))
                screen_damage.mark(b.x + float(r.x), b.y + float(r.y), b.x + float(r.x + r.w), b.y + float(r.y + r.h));
    }

    // A kept layer's texture over the screen.
    void blit_kept(const Batch& b) {
        auto it = kept_.find(b.layer);
        if (it == kept_.end() || !it->second.tex.id) return;
        const float x0 = b.x, y0 = b.y, x1 = b.x + float(it->second.tex.w), y1 = b.y + float(it->second.tex.h);
        const float big = 1e4f;
        Vertex v[4];
        const float px[4] = {x0, x1, x0, x1}, py[4] = {y0, y0, y1, y1};
        const float pu[4] = {0, 1, 0, 1}, pv[4] = {1, 1, 0, 0};  // the texture is upside down
        for (int i = 0; i < 4; ++i) {
            v[i].x = px[i], v[i].y = py[i], v[i].u = pu[i], v[i].v = pv[i];
            for (int k = 0; k < 4; ++k) v[i].edge[k] = v[i].clip[k] = big;
            std::memset(v[i].color, 255, 4);
            v[i].radius = 0, v[i].mode = 4;
        }
        glBindTexture(GL_TEXTURE_2D, it->second.tex.id);
        glUniform1f(u_native_, 0.f);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glBindBuffer(GL_ARRAY_BUFFER, blit_vbo_);
        glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
        set_attribs(0);
        glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    // The vertex attributes, read from the bound buffer starting at vertex `first`.
    void set_attribs(size_t first) {
        const auto* base = reinterpret_cast<const char*>(first * sizeof(Vertex));
        glVertexAttribPointer(GLuint(o_pos_), 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), base + offsetof(Vertex, x));
        glVertexAttribPointer(GLuint(o_uv_), 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), base + offsetof(Vertex, u));
        glVertexAttribPointer(GLuint(o_edge_), 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), base + offsetof(Vertex, edge));
        glVertexAttribPointer(GLuint(o_clip_), 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), base + offsetof(Vertex, clip));
        glVertexAttribPointer(GLuint(o_color_), 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex), base + offsetof(Vertex, color));
        glVertexAttribPointer(GLuint(o_param_), 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), base + offsetof(Vertex, radius));
    }

    void power(bool on) {
        force_full_ = true;
        hist_.clear();
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
    void enable_stats() { stats_ = true; }
    void enable_verify() { verify_ = true; }

private:
    static constexpr size_t kMaxBatchQuads = 16000;  // 16-bit indices
    static constexpr size_t kMaxParts = 48;          // separate rectangles drawn for one frame's damage
    bool stats_ = false;
    std::unordered_map<uint32_t, KeptLayer> kept_;
    std::unordered_map<uint32_t, TargetList> lists_;  // 0: the screen; else a kept layer
    uint32_t cur_target_ = 0;                          // while the commands are read
    bool force_full_ = true;                           // nothing to compare with: draw everything
    uint64_t unique_ = 0;
    struct AtlasWrite {
        int page, u, v, w, h;
    };
    std::vector<AtlasWrite> uploads_;  // this frame's writes into the mask atlas
    std::deque<Tiles> hist_;           // what the last frames changed on the screen (newest first)
    bool age_ext_ = false;
    bool verify_ = false;
    GLuint vbo_ = 0, blit_vbo_ = 0, ebo_ = 0;
    unsigned verified_ = 0, verify_bad_ = 0;
    Batch open_layer_;
    bool in_layer_ = false;
    unsigned stat_hit_ = 0, stat_miss_ = 0, gpu_n_hit_ = 0, gpu_n_miss_ = 0;
    double gpu_ms_hit_ = 0, gpu_ms_miss_ = 0;
    static double mono_now() {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
    }

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

    // FNV-style hash of memory, a word at a time.
    static uint64_t hash_mem(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
        const auto* b = static_cast<const uint8_t*>(p);
        size_t i = 0;
        for (; i + 8 <= n; i += 8) {
            uint64_t w;
            std::memcpy(&w, b + i, 8);
            h = (h ^ w) * 1099511628211ull;
            h ^= h >> 29;
        }
        for (; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
        return h;
    }

    // `texsig`: what the sampled pixels are (a hash of them, or the atlas page); `vol`: a
    // plugin's surface; `page`: an atlas page, whose uploads can change what is sampled.
    void quad(const Box& g, float u0, float v0, float u1, float v1, const Box& edge, uint32_t color, float radius,
              float mode, TexRef tex, uint64_t texsig = 0, bool vol = false, int page = -1,
              const uint32_t* vertex_colors = nullptr) {
        // Shapes do not sample: they join any batch. Masks and images need
        // their texture bound, so a batch takes them while it has none or the same.
        bool need = mode > 0.5f && mode < 2.5f;
        Batch* b = batches_.empty() || batches_.back().kind != Batch::Draw || batches_.back().target != cur_target_
                       ? nullptr
                       : &batches_.back();
        if (b && b->quads < kMaxBatchQuads && need && !(b->tex == tex)) {
            if (b->tex.kind == TexRef::None) b->tex = tex;
            else b = nullptr;
        }
        if (!b || b->quads >= kMaxBatchQuads) {
            Batch nb;
            nb.tex = need ? tex : TexRef{};
            nb.first = verts_.size();
            nb.target = cur_target_;
            batches_.push_back(nb);
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
            std::memcpy(v.color, vertex_colors ? &vertex_colors[i] : &color, 4);
            v.radius = radius, v.mode = mode;
            verts_.push_back(v);
        }
        ++b->quads;
        b->bx0 = std::min(b->bx0, g.x0), b->by0 = std::min(b->by0, g.y0);
        b->bx1 = std::max(b->bx1, g.x1), b->by1 = std::max(b->by1, g.y1);
        QInfo q{g.x0, g.y0, g.x1, g.y1, hash_mem(&verts_[verts_.size() - 4], 4 * sizeof(Vertex)) ^ (texsig * 0x9E3779B97F4A7C15ull)};
        q.vol = vol;
        q.page = page;
        q.u0 = u0, q.v0 = v0, q.u1 = u1, q.v1 = v1;
        lists_[cur_target_].cur.push_back(q);
    }

    // A kept layer laid over the screen, as the screen's list sees it.
    void note_blit(uint32_t id, float x, float y, float w, float h) {
        const float k[5] = {float(id), x, y, w, h};
        QInfo q{x, y, x + w, y + h, hash_mem(k, sizeof k, 0xB117ull)};
        lists_[0].cur.push_back(q);
    }

    // FACET_GPU_STATS: how much the quads of a frame cover, once in a while.
    void report_stats() {
        bool redrawn = false;
        for (const auto& b : batches_) redrawn = redrawn || b.kind == Batch::Begin;
        unsigned& n = redrawn ? stat_miss_ : stat_hit_;
        if (n++ % 60 != 1) return;
        std::fprintf(stderr, "gpu stats (a kept layer %s):\n", redrawn ? "was drawn" : "was reused");
        double total = 0, blended = 0, texd = 0;
        for (size_t i = 0; i + 3 < verts_.size(); i += 4) {
            const Vertex &a = verts_[i], &d = verts_[i + 3];
            double area = double(d.x - a.x) * double(d.y - a.y);
            total += area;
            if (d.mode > 0.5f) texd += area;
            if (a.color[3] < 255 || a.radius >= 0.5f || a.mode > 0.5f) blended += area;
        }
        double screen = double(w_) * double(h_);
        std::vector<std::pair<double, size_t>> big;
        for (size_t i = 0; i + 3 < verts_.size(); i += 4)
            big.push_back({double(verts_[i + 3].x - verts_[i].x) * double(verts_[i + 3].y - verts_[i].y), i});
        std::sort(big.rbegin(), big.rend());
        for (size_t k = 0; k < big.size() && k < 5; ++k) {
            const Vertex &a = verts_[big[k].second], &d = verts_[big[k].second + 3];
            std::fprintf(stderr, "  quad #%zu %.0fx%.0f at %.0f,%.0f = %.2fx, alpha %d, radius %.0f, mode %.0f\n", big[k].second / 4,
                         d.x - a.x, d.y - a.y, a.x, a.y, big[k].first / screen, a.color[3], a.radius, a.mode);
        }
        std::fprintf(stderr, "gpu stats: %zu quads, %zu batches, covers %.2fx the screen (%.2fx blended, %.2fx textured)\n",
                     verts_.size() / 4, batches_.size(), total / screen, blended / screen, texd / screen);
    }

    // Reads the commands into quads; uploads masks and images on the way.
    void parse_ops(size_t bytes) {
        verts_.clear();
        batches_.clear();
        for (auto& kv : lists_) kv.second.cur.clear();
        cur_target_ = 0;
        uploads_.clear();
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
                case ops::kGradient: {
                    if (payload < sizeof(ops::Gradient)) break;
                    const auto* o = reinterpret_cast<const ops::Gradient*>(rec);
                    Box shape{o->x, o->y, o->x + o->w, o->y + o->h_};
                    Box g{std::max(shape.x0 - 1, clip_.x0 - 1), std::max(shape.y0 - 1, clip_.y0 - 1),
                          std::min(shape.x1 + 1, clip_.x1 + 1), std::min(shape.y1 + 1, clip_.y1 + 1)};
                    if (g.x1 <= g.x0 || g.y1 <= g.y0) break;
                    // The colour of each corner of the quad, along the gradient.
                    auto mix = [&](float t) {
                        t = std::min(1.f, std::max(0.f, t));
                        uint32_t out = 0;
                        for (int ch = 0; ch < 4; ++ch) {
                            float a = float((o->from >> (8 * ch)) & 0xFF), b = float((o->to >> (8 * ch)) & 0xFF);
                            out |= uint32_t(a + (b - a) * t + 0.5f) << (8 * ch);
                        }
                        return out;
                    };
                    const float px[4] = {g.x0, g.x1, g.x0, g.x1}, py[4] = {g.y0, g.y0, g.y1, g.y1};
                    uint32_t vc[4];
                    for (int i = 0; i < 4; ++i)
                        vc[i] = o->vertical ? mix((py[i] - shape.y0) / std::max(1.f, shape.y1 - shape.y0))
                                            : mix((px[i] - shape.x0) / std::max(1.f, shape.x1 - shape.x0));
                    quad(g, 0, 0, 0, 0, shape, o->from, o->radius, 0, {}, 0, false, -1, vc);
                    break;
                }
                case ops::kEllipse: {
                    if (payload < sizeof(ops::Ellipse)) break;
                    const auto* o = reinterpret_cast<const ops::Ellipse*>(rec);
                    Box shape{o->x, o->y, o->x + o->w, o->y + o->h_};
                    Box g{std::max(shape.x0 - 1, clip_.x0 - 1), std::max(shape.y0 - 1, clip_.y0 - 1),
                          std::min(shape.x1 + 1, clip_.x1 + 1), std::min(shape.y1 + 1, clip_.y1 + 1)};
                    if (g.x1 <= g.x0 || g.y1 <= g.y0) break;
                    quad(g, 0, 0, 0, 0, shape, o->color, 0, 5, {});
                    break;
                }
                case ops::kGlow: {
                    if (payload < sizeof(ops::Glow)) break;
                    const auto* o = reinterpret_cast<const ops::Glow*>(rec);
                    Box shape{o->x, o->y, o->x + o->w, o->y + o->h_};
                    // The quad reaches as far as the light does, never past the clip.
                    float reach = o->spread + 1;
                    Box g{std::max(shape.x0 - reach, clip_.x0 - 1), std::max(shape.y0 - reach, clip_.y0 - 1),
                          std::min(shape.x1 + reach, clip_.x1 + 1), std::min(shape.y1 + reach, clip_.y1 + 1)};
                    if (g.x1 <= g.x0 || g.y1 <= g.y0) break;
                    quad(g, o->spread, 0, o->spread, 0, shape, o->color, o->radius, 3, {});
                    break;
                }
                case ops::kLayer: {
                    if (payload < sizeof(ops::Layer)) break;
                    const auto* o = reinterpret_cast<const ops::Layer*>(rec);
                    Batch m;
                    m.layer = o->id, m.x = o->x, m.y = o->y, m.w = o->w, m.h = o->h_;
                    if (o->use) {
                        if (kept_.count(o->id)) {
                            m.kind = Batch::Blit;
                            batches_.push_back(m);
                            note_blit(o->id, o->x, o->y, o->w, o->h_);
                        }
                    } else {
                        m.kind = Batch::Begin;
                        batches_.push_back(m);
                        open_layer_ = m;
                        in_layer_ = true;
                        cur_target_ = o->id;
                        clip_ = {0, 0, float(w_), float(h_)};
                    }
                    break;
                }
                case ops::kLayerEnd: {
                    if (!in_layer_) break;
                    in_layer_ = false;
                    Batch e = open_layer_;
                    e.kind = Batch::End;
                    batches_.push_back(e);
                    e.kind = Batch::Blit;
                    batches_.push_back(e);
                    cur_target_ = 0;
                    note_blit(e.layer, e.x, e.y, e.w, e.h);
                    clip_ = {0, 0, float(w_), float(h_)};
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
                    uploads_.push_back({int(o->page), o->u, o->v, o->w, o->h_});
                    break;
                }
                case ops::kAtlasReset: force_full_ = true; break;
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
                        quad(g, 0, 0, 1, 1, none, o->color, 0, 1, {TexRef::Inline, int(slot)},
                             hash_mem(o + 1, size_t(o->w) * size_t(o->h_)));
                    } else if (o->page < uint32_t(ops::kMaxAtlasPages)) {
                        const float s = 1.f / float(ops::kAtlasSize);
                        quad(g, float(o->u) * s, float(o->v) * s, float(o->u + o->w) * s, float(o->v + o->h_) * s, none,
                             o->color, 0, 1, {TexRef::Page, int(o->page)}, uint64_t(o->page) + 1, false, int(o->page));
                    }
                    break;
                }
                case ops::kSurface: {
                    if (payload < sizeof(ops::Surface)) break;
                    const auto* o = reinterpret_cast<const ops::Surface*>(rec);
                    if (o->layer >= layer_dst_.size()) break;
                    const Rect& d = layer_dst_[o->layer];
                    Box g{float(d.x), float(d.y), float(d.x + d.w), float(d.y + d.h)};
                    quad(g, 0, 0, 1, 1, none, 0xFFFFFFFFu, 0, 2, {TexRef::Layer, int(o->layer)}, 0, true);
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
                    quad(g, 0, 0, 1, 1, none, 0xFFFFFFFFu, 0, 2, {TexRef::Image, int(slot)},
                         hash_mem(o + 1, size_t(o->w) * size_t(o->h_) * 4));
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
        u_origin_ = glGetUniformLocation(ops_prog_, "origin");
        // GPU buffers of plugins: imported as EGL images.
        const char* egl_ext = eglQueryString(dpy_, EGL_EXTENSIONS);
        const char* gl_ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        create_image_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        destroy_image_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
        image_target_ =
            reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        age_ext_ = egl_ext && std::strstr(egl_ext, "EGL_EXT_buffer_age");
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
        glGenBuffers(1, &vbo_);
        glGenBuffers(1, &blit_vbo_);
        glGenBuffers(1, &ebo_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(quad_indices_.size() * sizeof(GLushort)), quad_indices_.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
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
          u_native_ = -1, u_origin_ = -1;
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
    if (std::getenv("FACET_GPU_STATS")) gpu.enable_stats();
    if (std::getenv("FACET_GPU_VERIFY")) gpu.enable_verify();
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
