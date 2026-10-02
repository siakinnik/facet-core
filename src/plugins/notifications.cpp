#include "plugins/notifications.h"

#include <algorithm>

namespace facet::plugins {

namespace {
constexpr size_t kMaxKept = 100;
constexpr double kDefaultRing = 45;
}  // namespace

Json Notification::to_json() const {
    Json j = Json::object();
    j["id"] = id;
    j["source"] = source;
    if (!app_name.empty()) j["app_name"] = app_name;
    j["title"] = title;
    j["body"] = body;
    j["kind"] = kind;
    j["priority"] = priority;
    if (!icon.empty()) j["icon"] = icon;
    Json acts = Json::array();
    for (const auto& [aid, label] : actions) {
        Json a = Json::object();
        a["id"] = aid;
        a["label"] = label;
        acts.push_back(a);
    }
    j["actions"] = acts;
    return j;
}

Notification Notification::from_json(const Json& j) {
    Notification n;
    auto cut = [](std::string s, size_t max) {
        if (s.size() > max) s.resize(max);
        return s;
    };
    n.id = cut(j["id"].str(), 128);
    n.title = cut(j["title"].str(), 200);
    n.body = cut(j["body"].str(), 2000);
    n.kind = j["kind"].as_string("message");
    if (n.kind != "message" && n.kind != "call" && n.kind != "alarm" && n.kind != "status") n.kind = "message";
    n.priority = j["priority"].as_string("normal");
    if (n.priority != "low" && n.priority != "normal" && n.priority != "high") n.priority = "normal";
    n.icon = cut(j["icon"].str(), 32);
    n.source = cut(j["source"].str(), 64);
    n.app_name = cut(j["app_name"].str(), 64);
    for (const auto& a : j["actions"].items()) {
        if (n.actions.size() >= 3) break;
        if (!a["id"].str().empty()) n.actions.emplace_back(cut(a["id"].str(), 64), cut(a["label"].str(), 40));
    }
    int timeout = j["timeout"].as_int(0);
    n.ring_until = timeout > 0 ? std::min(timeout, 300) : kDefaultRing;  // relative until posted
    return n;
}

const Notification& NotificationCenter::post(Notification n, double now) {
    cancel(n.poster, n.source, n.id);
    n.posted_at = now;
    n.serial = next_serial_++;
    n.banner_shown = n.kind == "status" || n.priority == "low";
    if (n.is_call()) n.ring_until = now + (n.ring_until > 0 ? n.ring_until : kDefaultRing);
    list_.push_front(std::move(n));
    while (list_.size() > kMaxKept) list_.pop_back();
    return list_.front();
}

bool NotificationCenter::cancel(const std::string& poster, const std::string& source, const std::string& id) {
    auto it = std::find_if(list_.begin(), list_.end(), [&](const Notification& n) {
        return n.poster == poster && n.source == source && n.id == id;
    });
    if (it == list_.end()) return false;
    list_.erase(it);
    return true;
}

std::optional<Notification> NotificationCenter::take(uint64_t serial) {
    auto it = std::find_if(list_.begin(), list_.end(), [&](const Notification& n) { return n.serial == serial; });
    if (it == list_.end()) return std::nullopt;
    Notification n = std::move(*it);
    list_.erase(it);
    return n;
}

void NotificationCenter::remove_calls_of(const std::string& poster) {
    list_.erase(std::remove_if(list_.begin(), list_.end(),
                               [&](const Notification& n) { return n.is_call() && n.poster == poster; }),
                list_.end());
}

Notification* NotificationCenter::find(uint64_t serial) {
    for (auto& n : list_)
        if (n.serial == serial) return &n;
    return nullptr;
}

Notification* NotificationCenter::ringing(double now) {
    for (auto& n : list_)
        if (n.is_call() && n.ring_until > now) return &n;
    return nullptr;
}

std::vector<Notification> NotificationCenter::expire_calls(double now) {
    std::vector<Notification> out;
    for (auto it = list_.begin(); it != list_.end();) {
        if (it->is_call() && it->ring_until <= now) {
            out.push_back(std::move(*it));
            it = list_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

std::optional<Notification> NotificationCenter::next_banner() {
    // Oldest unshown first, so a burst is shown in order.
    for (auto it = list_.rbegin(); it != list_.rend(); ++it) {
        if (!it->banner_shown && !it->is_call()) {
            it->banner_shown = true;
            return *it;
        }
    }
    return std::nullopt;
}

void NotificationCenter::clear() {
    // Ringing calls stay: they are answered or declined, not cleared.
    list_.erase(std::remove_if(list_.begin(), list_.end(), [](const Notification& n) { return !n.is_call(); }),
                list_.end());
}

}  // namespace facet::plugins
