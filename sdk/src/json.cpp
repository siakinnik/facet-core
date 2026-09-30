#include "facet/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace facet {

namespace {

const Json kNull;
const std::string kEmptyString;
const Json::Array kEmptyArray;
const Json::Object kEmptyObject;

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    bool parse(Json& out, std::string* err) {
        ws();
        if (!value(out, 0)) {
            if (err) *err = error_ + " at " + std::to_string(pos_);
            return false;
        }
        ws();
        if (pos_ != s_.size()) {
            if (err) *err = "trailing characters at " + std::to_string(pos_);
            return false;
        }
        return true;
    }

private:
    bool fail(const char* msg) {
        error_ = msg;
        return false;
    }

    void ws() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
    }

    bool literal(std::string_view lit) {
        if (s_.substr(pos_, lit.size()) != lit) return false;
        pos_ += lit.size();
        return true;
    }

    bool value(Json& out, int depth) {
        if (depth > 64) return fail("nesting too deep");
        if (pos_ >= s_.size()) return fail("unexpected end");
        char c = s_[pos_];
        if (c == '{') return object(out, depth);
        if (c == '[') return array(out, depth);
        if (c == '"') {
            std::string str;
            if (!string(str)) return false;
            out = Json(std::move(str));
            return true;
        }
        if (literal("true")) { out = Json(true); return true; }
        if (literal("false")) { out = Json(false); return true; }
        if (literal("null")) { out = Json(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return number(out);
        return fail("unexpected character");
    }

    bool number(Json& out) {
        size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')
                ++pos_;
            else
                break;
        }
        std::string tmp(s_.substr(start, pos_ - start));
        char* end = nullptr;
        double v = std::strtod(tmp.c_str(), &end);
        if (end == tmp.c_str() || *end != '\0') return fail("bad number");
        out = Json(v);
        return true;
    }

    static void append_utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(uint32_t& v) {
        if (pos_ + 4 > s_.size()) return fail("bad escape");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return fail("bad escape");
        }
        return true;
    }

    bool string(std::string& out) {
        ++pos_;  // opening quote
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) break;
            char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && s_.substr(pos_, 2) == "\\u") {
                        pos_ += 2;
                        uint32_t lo;
                        if (!hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    append_utf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool array(Json& out, int depth) {
        ++pos_;
        out = Json::array();
        ws();
        if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
        while (true) {
            Json item;
            ws();
            if (!value(item, depth + 1)) return false;
            out.push_back(std::move(item));
            ws();
            if (pos_ >= s_.size()) return fail("unterminated array");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; return true; }
            return fail("expected , or ]");
        }
    }

    bool object(Json& out, int depth) {
        ++pos_;
        out = Json::object();
        ws();
        if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
        while (true) {
            ws();
            if (pos_ >= s_.size() || s_[pos_] != '"') return fail("expected key");
            std::string key;
            if (!string(key)) return false;
            ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') return fail("expected :");
            ++pos_;
            ws();
            Json v;
            if (!value(v, depth + 1)) return false;
            out[key] = std::move(v);
            ws();
            if (pos_ >= s_.size()) return fail("unterminated object");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; return true; }
            return fail("expected , or }");
        }
    }

    std::string_view s_;
    size_t pos_ = 0;
    std::string error_;
};

void dump_string(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += char(c);
                }
        }
    }
    out += '"';
}

}  // namespace

const std::string& Json::str() const { return type_ == Type::String ? str_ : kEmptyString; }
const Json::Array& Json::items() const { return type_ == Type::Array ? arr_ : kEmptyArray; }
const Json::Object& Json::fields() const { return type_ == Type::Object ? obj_ : kEmptyObject; }

const Json& Json::operator[](const std::string& key) const {
    if (type_ != Type::Object) return kNull;
    auto it = obj_.find(key);
    return it == obj_.end() ? kNull : it->second;
}

Json& Json::operator[](const std::string& key) {
    if (type_ != Type::Object) *this = object();
    return obj_[key];
}

bool Json::contains(const std::string& key) const {
    return type_ == Type::Object && obj_.count(key) != 0;
}

const Json& Json::operator[](size_t i) const {
    if (type_ != Type::Array || i >= arr_.size()) return kNull;
    return arr_[i];
}

void Json::push_back(Json v) {
    if (type_ != Type::Array) *this = array();
    arr_.push_back(std::move(v));
}

size_t Json::size() const {
    if (type_ == Type::Array) return arr_.size();
    if (type_ == Type::Object) return obj_.size();
    return 0;
}

bool Json::operator==(const Json& o) const {
    if (type_ != o.type_) return false;
    switch (type_) {
        case Type::Null: return true;
        case Type::Bool: return bool_ == o.bool_;
        case Type::Number: return num_ == o.num_;
        case Type::String: return str_ == o.str_;
        case Type::Array: return arr_ == o.arr_;
        case Type::Object: return obj_ == o.obj_;
    }
    return false;
}

void Json::dump_to(std::string& out) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            char buf[32];
            if (std::isfinite(num_) && num_ == std::floor(num_) && std::fabs(num_) < 1e15)
                std::snprintf(buf, sizeof buf, "%lld", (long long)num_);
            else if (std::isfinite(num_))
                std::snprintf(buf, sizeof buf, "%.10g", num_);
            else
                std::snprintf(buf, sizeof buf, "null");
            out += buf;
            break;
        }
        case Type::String: dump_string(out, str_); break;
        case Type::Array: {
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += ',';
                arr_[i].dump_to(out);
            }
            out += ']';
            break;
        }
        case Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& [k, v] : obj_) {
                if (!first) out += ',';
                first = false;
                dump_string(out, k);
                out += ':';
                v.dump_to(out);
            }
            out += '}';
            break;
        }
    }
}

std::string Json::dump() const {
    std::string out;
    dump_to(out);
    return out;
}

bool Json::parse(std::string_view text, Json& out, std::string* error) {
    return Parser(text).parse(out, error);
}

bool load_json_file(const std::string& path, Json& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    return Json::parse(ss.str(), out);
}

bool save_json_file(const std::string& path, const Json& value) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << value.dump() << '\n';
        f.flush();
        if (!f) return false;
    }
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace facet
