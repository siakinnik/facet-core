// Plugin-side SDK: message loop over stdin/stdout (protocol v1, see
// docs/ARCHITECTURE.md) and a builder for declarative screens.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "facet/i18n.h"
#include "facet/json.h"

namespace facet::sdk {

constexpr int kApiVersion = 1;

// Builds the UI tree a plugin sends to the core. Widget set mirrors ui::Context.
class Screen {
public:
    explicit Screen(std::string title);

    Screen& section(std::string title);
    Screen& info(std::string label, std::string value, std::string tone = "normal");
    Screen& note(std::string text);
    Screen& toggle(std::string id, std::string label, bool value);
    Screen& select(std::string id, std::string label, std::vector<std::string> options, int value);
    Screen& stepper(std::string id, std::string label, int value, int min, int max, int step,
                    std::string unit = {});
    Screen& time(std::string id, std::string label, int minutes, int step = 15);
    Screen& level(std::string label, double value, std::string text = {});
    Screen& button(std::string id, std::string label, std::string style = "normal");

    const Json& json() const { return root_; }

private:
    Json& add(const char* type);
    Json root_;
};

class Plugin {
public:
    Plugin(std::string id, std::string version);

    // Callbacks. All run on the thread that called run().
    std::function<void(const Json& hello)> on_hello;
    std::function<void(const std::string& id, const Json& value)> on_event;
    std::function<void(bool visible)> on_visible;
    // Called after the UI language changed (catalog already switched).
    std::function<void(const std::string& lang)> on_locale;
    std::function<void()> on_activity;
    std::function<void()> on_tick;
    std::function<void()> on_shutdown;

    const std::string& data_dir() const { return data_dir_; }

    // Register translation tables here; the language follows the core.
    i18n::Catalog& catalog() { return catalog_; }
    std::string tr(std::string_view key) const { return catalog_.tr(key); }
    std::string tr(std::string_view key, const std::vector<std::string>& args) const { return catalog_.tr(key, args); }
    std::string render(const i18n::Text& t) const { return catalog_.render(t); }
    bool visible() const { return visible_; }

    // Senders skip messages identical to the previously sent value.
    void set_ui(const Screen& screen);
    void set_tile(const std::string& subtitle);
    void request_display(bool on);
    void send(const Json& msg);

    // Runs until the core sends shutdown or closes stdin. Returns exit code.
    int run(int tick_ms = 250);
    void quit() { running_ = false; }

    static void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

private:
    void handle(const Json& msg);

    void set_locale(const std::string& lang);

    std::string id_, version_, data_dir_;
    i18n::Catalog catalog_;
    bool visible_ = false;
    bool running_ = true;
    Json last_ui_;
    std::string last_tile_;
    int last_display_ = -1;
};

}  // namespace facet::sdk
