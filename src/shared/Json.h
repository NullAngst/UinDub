// SPDX-License-Identifier: GPL-3.0-or-later
// Copied from PingDub (github.com/NullAngst/PingDub, same author, GPL-3.0-or-later).
// Small JSON reader for PingDub's own sidecars. Not a general library: no
// streaming, numbers are doubles, and invalid input simply fails.
// UinDub adds builders and a writer, so project files round-trip.
#pragma once

#include <map>
#include <memory>
#include <cstdint>
#include <string>
#include <vector>

namespace ud {

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JsonValue> a;
    std::map<std::string, JsonValue> o;

    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }
    bool has(const std::string& k) const { return type == Type::Object && o.count(k); }
    // Missing keys give a shared null value, so lookups chain safely.
    const JsonValue& operator[](const std::string& k) const;
    const JsonValue& operator[](size_t i) const;
    size_t size() const { return type == Type::Array ? a.size() : type == Type::Object ? o.size() : 0; }
    std::string str(const std::string& def = {}) const { return type == Type::String ? s : def; }
    double num(double def = 0) const { return type == Type::Number ? n : type == Type::Bool ? (b ? 1 : 0) : def; }
    long long integer(long long def = 0) const { return type == Type::Number ? (long long)n : def; }
    bool boolean(bool def = false) const { return type == Type::Bool ? b : def; }

    static bool parse(const std::string& text, JsonValue& out, std::string& err);

    // Builders (UinDub).
    static JsonValue object();
    static JsonValue array();
    JsonValue(double v);
    JsonValue(int v) : JsonValue(double(v)) {}
    JsonValue(long long v) : JsonValue(double(v)) {}
    JsonValue(long v) : JsonValue(double(v)) {}
    JsonValue(unsigned v) : JsonValue(double(v)) {}
    JsonValue(unsigned long v) : JsonValue(double(v)) {}
    JsonValue(bool v);
    JsonValue(const char* v);
    JsonValue(const std::string& v);
    JsonValue() = default;
    JsonValue& set(const std::string& k, JsonValue v);
    JsonValue& push(JsonValue v);
    // Text. Pretty prints with two-space indents.
    std::string dump(bool pretty = true) const;
};

bool writeJsonFile(const std::string& path, const JsonValue& v, std::string& err);

bool readJsonFile(const std::string& path, JsonValue& out, std::string& err);

}  // namespace ud
