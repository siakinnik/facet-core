// Immediate-mode UI API. Built-in screens and the plugin UI renderer both
// describe screens through this class, so every screen shares one look.
//
//   if (ui.begin_screen("settings", tr("Settings")) == ui::HeaderHit::Back) back();
//   ui.section(tr("Appearance"));
//   ui.select("theme", tr("Theme"), {tr("Dark"), tr("Light"), tr("Auto")}, theme);
//   ui.end_screen();
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "gfx/canvas.h"
#include "ui/icons.h"
#include "ui/theme.h"

namespace facet::ui {

enum class Tone { Normal, Dim, Good, Warn, Bad };
enum class ButtonStyle { Normal, Primary, Danger };
enum class HeaderHit { None, Back, Action };

struct Pointer {
    float x = 0, y = 0;
    bool down = false;
    bool pressed = false;   // went down this frame
    bool released = false;  // went up this frame
};

class Context {
public:
    void begin_frame(gfx::Canvas& canvas, const Theme& theme, const Pointer& pointer, double now);
    void end_frame();
    // True when the UI needs another frame (animation, fling, layout settle).
    bool wants_redraw() const { return redraw_; }
    // Drops per-screen interaction state (call when navigating).
    void reset_interaction();

    const Theme& theme() const { return *theme_; }
    gfx::Canvas& canvas() { return *canvas_; }
    gfx::Color tone_color(Tone tone) const;

    // ---- Screen scaffold: header + scrolling content column.
    HeaderHit begin_screen(std::string_view id, std::string_view title, bool back = true,
                           Icon action = Icon::None);
    void end_screen();

    // ---- List widgets (inside begin_screen/end_screen).
    void section(std::string_view title);
    void info(std::string_view label, std::string_view value, Tone tone = Tone::Normal);
    void note(std::string_view text);
    bool toggle(std::string_view id, std::string_view label, bool& value);
    bool select(std::string_view id, std::string_view label, const std::vector<std::string>& options,
                int& index);
    bool stepper(std::string_view id, std::string_view label, int& value, int min, int max, int step,
                 std::string_view unit = {});
    bool time(std::string_view id, std::string_view label, int& minutes, int step = 15);
    void level(std::string_view label, float value, std::string_view text = {});
    bool button(std::string_view id, std::string_view label, ButtonStyle style = ButtonStyle::Normal);

    // ---- Free-layout building blocks (menus, dashboards).
    bool tile(std::string_view id, const gfx::Rect& r, std::string_view title, std::string_view subtitle,
              Icon icon, Tone tone = Tone::Normal);
    bool icon_button(std::string_view id, const gfx::Rect& r, Icon icon);
    // A tap that no widget claimed.
    bool background_tap() const;

    void text(FontRole role, float size_dp, const gfx::Rect& box, std::string_view s, gfx::Color color,
              gfx::Align align = gfx::Align::Start);
    float text_width(FontRole role, float size_dp, std::string_view s);
    std::string ellipsize(FontRole role, float size_dp, std::string_view s, float max_w);
    std::vector<std::string> wrap(FontRole role, float size_dp, std::string_view s, float max_w);

private:
    struct Press {
        bool held = false;
        bool clicked = false;
    };
    struct Scroll {
        float offset = 0, velocity = 0, content_h = 0, start_offset = 0;
        double last_move = 0;
        bool tracking = false;
    };
    struct Popup {
        bool open = false;
        uint64_t id = 0;
        std::string title;
        std::vector<std::string> options;
        int current = 0;
        bool outside_press = false;
        // Long lists scroll by dragging; `fresh` centres the current item once.
        float scroll = 0, scroll_start = 0;
        bool tracking = false, fresh = true;
    };

    uint64_t make_id(std::string_view s) const;
    Press interact(uint64_t id, const gfx::Rect& r);
    int repeat_steps(uint64_t id, const gfx::Rect& r);  // tap = 1, hold = auto-repeat
    gfx::Rect row(float height_dp);
    void open_group(std::string_view title);
    void close_group();
    void row_highlight(const gfx::Rect& r, bool held);
    void draw_popup();
    bool round_button(uint64_t id, const gfx::Rect& r, bool plus, bool enabled, int& steps);

    gfx::Canvas* canvas_ = nullptr;
    const Theme* theme_ = nullptr;
    Pointer p_;
    double now_ = 0, dt_ = 0;
    bool redraw_ = false;
    bool blocked_ = false;

    uint64_t active_ = 0;
    float press_x_ = 0, press_y_ = 0;
    bool drag_ = false;
    double hold_start_ = 0, next_repeat_ = 0;
    bool repeated_ = false;

    // Current screen layout.
    uint64_t screen_ = 0;
    gfx::Rect viewport_;
    float col_x_ = 0, col_w_ = 0, cursor_ = 0, content_top_ = 0;
    bool in_group_ = false;
    int group_index_ = 0, group_rows_ = 0;
    float group_start_ = 0;
    uint64_t group_key_ = 0;

    std::unordered_map<uint64_t, Scroll> scroll_;
    std::unordered_map<uint64_t, float> group_h_;
    Popup popup_;
    uint64_t popup_result_id_ = 0;
    int popup_result_ = -1;
};

}  // namespace facet::ui
