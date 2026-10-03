// The link between the core and its GPU helper (facet-gpu): lines of JSON
// over a Unix stream socket; a message flagged "fd": true carries one file
// descriptor (SCM_RIGHTS). Used by both sides.
#pragma once

#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "facet/json.h"

namespace facet::gpu {

class Channel {
public:
    ~Channel() { close(); }
    void adopt(int fd, bool nonblocking);
    void close();
    int fd() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    // Sends one message, with `pass_fd` attached when >= 0 (and "fd": true set).
    bool send(Json msg, int pass_fd = -1);
    // Reads what is available (blocking channels: at least one message, up
    // to `timeout_ms`). Messages come with the descriptor they carried, -1 if
    // none. False when the peer went away.
    bool read(std::vector<std::pair<Json, int>>& out, int timeout_ms = 0);

private:
    bool fill(int timeout_ms);
    int fd_ = -1;
    std::string in_;
    std::deque<int> fds_;
};

// Protocol, core -> helper:
//   {"t": "ops", "size", "fd": true}               shared memory for drawing commands (gfx/ops.h),
//      sent instead of "canvas" when the helper said "ops": the helper draws the interface
//   {"t": "canvas", "w", "h", "fd": true}           the core's canvas (XRGB8888, alpha 0 = see-through)
//   {"t": "surface", "key", "w", "h", "stride", "buffers", "fd": true}   a plugin surface's buffers
//   {"t": "surface_drop", "key"}
//   {"t": "frame", "seq", "canvas": [[x, y, w, h], ...] (changed parts, may be empty),
//    "layers": [{"key", "buffer", "dst": [x, y, w, h], "clip": [x, y, w, h], "fresh": bool}]}
//      layers are drawn bottom to top under the canvas; with commands, "ops": bytes (new
//      commands in the shared memory; absent: the last ones again) and the layers are drawn
//      where the commands' Surface records say
//   {"t": "power", "on": bool}
//   {"t": "quit"}
// helper -> core:
//   {"t": "ready", "w", "h", "gpu": "...", "renderer": "...", "ops": bool} or {"t": "error", "message"}
//   {"t": "uploaded", "seq"}   textures are updated (commands read); the buffers of this frame may be reused
//   {"t": "shown", "seq"}      the frame is on screen (next frame may come)

}  // namespace facet::gpu
