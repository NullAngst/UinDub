// SPDX-License-Identifier: GPL-3.0-or-later
// Copied from PingDub (github.com/NullAngst/PingDub, same author, GPL-3.0-or-later).
#include "Json.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>

namespace ud {

namespace {

struct Parser {
    explicit Parser(const std::string& text) : t(text) {}
    const std::string& t;
    size_t i = 0;
    std::string err;
    int depth = 0;

    void ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
    }
    bool fail(const char* m) {
        if (err.empty()) err = std::string(m) + " at offset " + std::to_string(i);
        return false;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += char(cp);
        else if (cp < 0x800) {
            o += char(0xC0 | (cp >> 6));
            o += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            o += char(0xE0 | (cp >> 12));
            o += char(0x80 | ((cp >> 6) & 0x3F));
            o += char(0x80 | (cp & 0x3F));
        } else {
            o += char(0xF0 | (cp >> 18));
            o += char(0x80 | ((cp >> 12) & 0x3F));
            o += char(0x80 | ((cp >> 6) & 0x3F));
            o += char(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& v) {
        if (i + 4 > t.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = t[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& o) {
        ++i;  // opening quote
        while (i < t.size()) {
            const char c = t[i++];
            if (c == '"') return true;
            if (c != '\\') {
                o += c;
                continue;
            }
            if (i >= t.size()) break;
            const char e = t[i++];
            switch (e) {
            case '"': o += '"'; break;
            case '\\': o += '\\'; break;
            case '/': o += '/'; break;
            case 'b': o += '\b'; break;
            case 'f': o += '\f'; break;
            case 'n': o += '\n'; break;
            case 'r': o += '\r'; break;
            case 't': o += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(cp)) return fail("bad \\u escape");
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                    i += 2;
                    unsigned lo = 0;
                    if (!hex4(lo)) return fail("bad surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(o, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool value(JsonValue& v) {
        if (++depth > 64) return fail("nested too deep");
        ws();
        if (i >= t.size()) return fail("unexpected end");
        const char c = t[i];
        bool ok = true;
        if (c == '{') {
            v.type = JsonValue::Type::Object;
            ++i;
            ws();
            if (i < t.size() && t[i] == '}') {
                ++i;
            } else {
                while (ok) {
                    ws();
                    if (i >= t.size() || t[i] != '"') {
                        ok = fail("expected a key");
                        break;
                    }
                    std::string k;
                    if (!string(k)) {
                        ok = false;
                        break;
                    }
                    ws();
                    if (i >= t.size() || t[i] != ':') {
                        ok = fail("expected ':'");
                        break;
                    }
                    ++i;
                    JsonValue child;
                    if (!value(child)) {
                        ok = false;
                        break;
                    }
                    v.o[k] = std::move(child);
                    ws();
                    if (i < t.size() && t[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < t.size() && t[i] == '}') {
                        ++i;
                        break;
                    }
                    ok = fail("expected ',' or '}'");
                }
            }
        } else if (c == '[') {
            v.type = JsonValue::Type::Array;
            ++i;
            ws();
            if (i < t.size() && t[i] == ']') {
                ++i;
            } else {
                while (ok) {
                    JsonValue child;
                    if (!value(child)) {
                        ok = false;
                        break;
                    }
                    v.a.push_back(std::move(child));
                    ws();
                    if (i < t.size() && t[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < t.size() && t[i] == ']') {
                        ++i;
                        break;
                    }
                    ok = fail("expected ',' or ']'");
                }
            }
        } else if (c == '"') {
            v.type = JsonValue::Type::String;
            ok = string(v.s);
        } else if (t.compare(i, 4, "true") == 0) {
            v.type = JsonValue::Type::Bool;
            v.b = true;
            i += 4;
        } else if (t.compare(i, 5, "false") == 0) {
            v.type = JsonValue::Type::Bool;
            i += 5;
        } else if (t.compare(i, 4, "null") == 0) {
            v.type = JsonValue::Type::Null;
            i += 4;
        } else {
            const char* start = t.c_str() + i;
            char* end = nullptr;
            v.n = std::strtod(start, &end);
            if (end == start) ok = fail("unexpected character");
            else {
                v.type = JsonValue::Type::Number;
                i += size_t(end - start);
            }
        }
        --depth;
        return ok;
    }
};

}  // namespace

const JsonValue& JsonValue::operator[](const std::string& k) const {
    static const JsonValue null;
    if (type != Type::Object) return null;
    auto it = o.find(k);
    return it == o.end() ? null : it->second;
}

const JsonValue& JsonValue::operator[](size_t idx) const {
    static const JsonValue null;
    return type == Type::Array && idx < a.size() ? a[idx] : null;
}

bool JsonValue::parse(const std::string& text, JsonValue& out, std::string& err) {
    Parser p(text);
    out = JsonValue{};
    if (!p.value(out)) {
        err = p.err;
        return false;
    }
    p.ws();
    if (p.i != text.size()) {
        err = "trailing data after the JSON";
        return false;
    }
    return true;
}

bool readJsonFile(const std::string& path, JsonValue& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        err = "cannot open " + path;
        return false;
    }
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    std::fclose(f);
    return JsonValue::parse(text, out, err);
}

JsonValue JsonValue::object() {
    JsonValue v;
    v.type = Type::Object;
    return v;
}
JsonValue JsonValue::array() {
    JsonValue v;
    v.type = Type::Array;
    return v;
}
JsonValue::JsonValue(double v) : type(Type::Number), n(v) {}
JsonValue::JsonValue(bool v) : type(Type::Bool), b(v) {}
JsonValue::JsonValue(const char* v) : type(Type::String), s(v ? v : "") {}
JsonValue::JsonValue(const std::string& v) : type(Type::String), s(v) {}

JsonValue& JsonValue::set(const std::string& k, JsonValue v) {
    if (type != Type::Object) {
        *this = object();
    }
    o[k] = std::move(v);
    return *this;
}

JsonValue& JsonValue::push(JsonValue v) {
    if (type != Type::Array) *this = array();
    a.push_back(std::move(v));
    return *this;
}

static void escapeTo(std::string& o, const std::string& s) {
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", c);
                o += b;
            } else {
                o += char(c);
            }
        }
    }
    o += '"';
}

static void dumpTo(std::string& o, const JsonValue& v, bool pretty, int depth) {
    auto nl = [&](int d) {
        if (!pretty) return;
        o += '\n';
        o.append(size_t(d) * 2, ' ');
    };
    switch (v.type) {
    case JsonValue::Type::Null: o += "null"; break;
    case JsonValue::Type::Bool: o += v.b ? "true" : "false"; break;
    case JsonValue::Type::Number: {
        char b[40];
        if (std::isfinite(v.n) && v.n == std::floor(v.n) && std::fabs(v.n) < 9.0e15) std::snprintf(b, sizeof b, "%.0f", v.n);
        else if (std::isfinite(v.n)) std::snprintf(b, sizeof b, "%.10g", v.n);
        else std::snprintf(b, sizeof b, "null");
        o += b;
        break;
    }
    case JsonValue::Type::String: escapeTo(o, v.s); break;
    case JsonValue::Type::Array: {
        if (v.a.empty()) {
            o += "[]";
            break;
        }
        // Arrays of plain numbers stay on one line, so offset tables do not run for pages.
        bool flat = true;
        for (const JsonValue& e : v.a) flat &= e.type == JsonValue::Type::Number || e.type == JsonValue::Type::Bool;
        o += '[';
        for (size_t i = 0; i < v.a.size(); ++i) {
            if (i) o += pretty && flat ? ", " : ",";
            if (!flat) nl(depth + 1);
            dumpTo(o, v.a[i], pretty, depth + 1);
        }
        if (!flat) nl(depth);
        o += ']';
        break;
    }
    case JsonValue::Type::Object: {
        if (v.o.empty()) {
            o += "{}";
            break;
        }
        o += '{';
        bool first = true;
        for (const auto& [k, e] : v.o) {
            if (!first) o += ',';
            first = false;
            nl(depth + 1);
            escapeTo(o, k);
            o += pretty ? ": " : ":";
            dumpTo(o, e, pretty, depth + 1);
        }
        nl(depth);
        o += '}';
        break;
    }
    }
}

std::string JsonValue::dump(bool pretty) const {
    std::string o;
    dumpTo(o, *this, pretty, 0);
    if (pretty) o += '\n';
    return o;
}

bool writeJsonFile(const std::string& path, const JsonValue& v, std::string& err) {
    // Written next to the target and renamed over it, so a crash never leaves half a file.
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) {
        err = "cannot write " + path;
        return false;
    }
    const std::string text = v.dump(true);
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        err = "cannot write " + path;
        return false;
    }
    return true;
}

}  // namespace ud
