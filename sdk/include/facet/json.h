// Minimal JSON value, parser and serializer. Shared by the core and plugins.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace facet {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool v) : type_(Type::Bool), bool_(v) {}
    Json(int v) : type_(Type::Number), num_(v) {}
    Json(long v) : type_(Type::Number), num_(double(v)) {}
    Json(long long v) : type_(Type::Number), num_(double(v)) {}
    Json(unsigned v) : type_(Type::Number), num_(v) {}
    Json(double v) : type_(Type::Number), num_(v) {}
    Json(float v) : type_(Type::Number), num_(v) {}
    Json(const char* v) : type_(Type::String), str_(v) {}
    Json(std::string v) : type_(Type::String), str_(std::move(v)) {}
    Json(std::string_view v) : type_(Type::String), str_(v) {}
    Json(Array v) : type_(Type::Array), arr_(std::move(v)) {}
    Json(Object v) : type_(Type::Object), obj_(std::move(v)) {}

    static Json array() { return Json(Array{}); }
    static Json object() { return Json(Object{}); }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    double as_number(double def = 0) const { return type_ == Type::Number ? num_ : def; }
    int as_int(int def = 0) const { return type_ == Type::Number ? int(num_) : def; }
    std::string as_string(std::string_view def = {}) const {
        return type_ == Type::String ? str_ : std::string(def);
    }
    const std::string& str() const;   // empty string if not a string
    const Array& items() const;       // empty if not an array
    const Object& fields() const;     // empty if not an object

    // Object access. The const version returns null for missing keys.
    const Json& operator[](const std::string& key) const;
    Json& operator[](const std::string& key);  // converts to object
    bool contains(const std::string& key) const;

    // Array access.
    const Json& operator[](size_t i) const;
    void push_back(Json v);  // converts to array
    Json& back() { return arr_.back(); }
    Json& at(size_t i) { return arr_.at(i); }  // throws if out of range / not an array
    size_t size() const;

    bool operator==(const Json& o) const;
    bool operator!=(const Json& o) const { return !(*this == o); }

    std::string dump() const;
    static bool parse(std::string_view text, Json& out, std::string* error = nullptr);

private:
    void dump_to(std::string& out) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0;
    std::string str_;
    Array arr_;
    Object obj_;
};

// Reads/writes a JSON file. save_json_file writes atomically (tmp + rename).
bool load_json_file(const std::string& path, Json& out);
bool save_json_file(const std::string& path, const Json& value);

}  // namespace facet
