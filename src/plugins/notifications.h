// Notifications posted by plugins: the list behind the bell in the status
// bar, heads-up banners and incoming calls. In memory only.
#pragma once

#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "facet/json.h"

namespace facet::plugins {

struct Notification {
    std::string poster;    // plugin that posted it; actions go back to it
    std::string source;    // module shown as the sender (= poster unless a distributor posted it)
    std::string app_name;  // set for apps that are no module (e.g. Wayland clients)
    std::string id;        // the poster's id; (poster, source, id) is unique
    std::string title, body, kind, priority, icon;
    std::vector<std::pair<std::string, std::string>> actions;  // id, label
    double posted_at = 0;  // steady seconds
    double ring_until = 0; // calls only
    bool banner_shown = false;
    uint64_t serial = 0;   // increases with every post (for "new since")

    bool is_call() const { return kind == "call"; }
    Json to_json() const;  // as sent to distributors
    static Notification from_json(const Json& j);
};

class NotificationCenter {
public:
    // Adds or replaces (same poster, source and id). Returns the stored one.
    const Notification& post(Notification n, double now);
    bool cancel(const std::string& poster, const std::string& source, const std::string& id);
    // Removes and returns it (e.g. when the user acts on it).
    std::optional<Notification> take(uint64_t serial);
    void remove_calls_of(const std::string& poster);

    const std::deque<Notification>& list() const { return list_; }  // newest first
    Notification* find(uint64_t serial);
    // The call that is ringing now, if any (the newest).
    Notification* ringing(double now);
    // Calls whose ring time ran out (removed from the list).
    std::vector<Notification> expire_calls(double now);
    // Next notification that should appear as a banner (marks it shown).
    std::optional<Notification> next_banner();
    void clear();

private:
    std::deque<Notification> list_;
    uint64_t next_serial_ = 1;
};

}  // namespace facet::plugins
