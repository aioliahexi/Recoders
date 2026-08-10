// 极简 JSON：解析 + 序列化（零依赖，够 REST/配置使用）
#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace camera::json {

class Value {
public:
    enum class Type { Null, Bool, Int, Double, String, Array, Object };

    Value() : type_(Type::Null) {}
    Value(std::nullptr_t) : type_(Type::Null) {}
    Value(bool b) : type_(Type::Bool), bool_(b) {}
    Value(int64_t i) : type_(Type::Int), int_(i) {}
    Value(int i) : Value(static_cast<int64_t>(i)) {}
    Value(double d) : type_(Type::Double), dbl_(d) {}
    Value(const char* s) : Value(std::string(s)) {}
    Value(std::string s) : type_(Type::String), str_(std::move(s)) {}

    static Value array() { Value v; v.type_ = Type::Array; return v; }
    static Value object() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }

    bool as_bool(bool d = false) const { return type_ == Type::Bool ? bool_ : d; }
    int64_t as_int(int64_t d = 0) const {
        if (type_ == Type::Int) return int_;
        if (type_ == Type::Double) return static_cast<int64_t>(dbl_);
        return d;
    }
    double as_double(double d = 0.0) const {
        if (type_ == Type::Double) return dbl_;
        if (type_ == Type::Int) return static_cast<double>(int_);
        return d;
    }
    const std::string& as_string(const std::string& d = "") const {
        return type_ == Type::String ? str_ : d;
    }

    Value& operator[](const std::string& key) {
        if (type_ != Type::Object) { *this = object(); }
        return obj_[key];
    }
    Value& operator[](size_t idx) {
        if (type_ != Type::Array) { *this = array(); arr_.resize(idx + 1); }
        if (arr_.size() <= idx) arr_.resize(idx + 1);
        return arr_[idx];
    }
    Value& push_back(Value v) {
        if (type_ != Type::Array) *this = array();
        arr_.push_back(std::move(v));
        return arr_.back();
    }
    bool contains(const std::string& key) const {
        return type_ == Type::Object && obj_.count(key) > 0;
    }
    const Value& get(const std::string& key) const {
        static const Value kNull;
        if (type_ != Type::Object) return kNull;
        auto it = obj_.find(key);
        return it == obj_.end() ? kNull : it->second;
    }
    size_t size() const {
        if (type_ == Type::Array) return arr_.size();
        if (type_ == Type::Object) return obj_.size();
        return 0;
    }
    const std::vector<Value>& items() const { return arr_; }
    const std::map<std::string, Value>& members() const { return obj_; }

private:
    Type type_ = Type::Null;
    bool bool_ = false;
    int64_t int_ = 0;
    double dbl_ = 0.0;
    std::string str_;
    std::vector<Value> arr_;
    std::map<std::string, Value> obj_;
};

inline std::string escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else out += c;
        }
    }
    return out;
}

inline std::string dump(const Value& v) {
    switch (v.type()) {
        case Value::Type::Null: return "null";
        case Value::Type::Bool: return v.as_bool() ? "true" : "false";
        case Value::Type::Int: return std::to_string(v.as_int());
        case Value::Type::Double: {
            double d = v.as_double();
            if (std::isfinite(d)) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.6g", d);
                return buf;
            }
            return "null";
        }
        case Value::Type::String: return "\"" + escape(v.as_string()) + "\"";
        case Value::Type::Array: {
            std::string out = "[";
            bool first = true;
            for (const auto& it : v.items()) {
                if (!first) out += ",";
                out += dump(it);
                first = false;
            }
            return out + "]";
        }
        case Value::Type::Object: {
            std::string out = "{";
            bool first = true;
            for (const auto& [k, val] : v.members()) {
                if (!first) out += ",";
                out += "\"" + escape(k) + "\":" + dump(val);
                first = false;
            }
            return out + "}";
        }
    }
    return "null";
}

// 带缩进的序列化（配置文件手编友好），indent 空格数
inline std::string dump_pretty(const Value& v, int indent = 2) {
    auto pad = [](int n) { return std::string(static_cast<size_t>(n), ' '); };
    switch (v.type()) {
        case Value::Type::Null: return "null";
        case Value::Type::Bool: return v.as_bool() ? "true" : "false";
        case Value::Type::Int: return std::to_string(v.as_int());
        case Value::Type::Double: {
            double d = v.as_double();
            if (std::isfinite(d)) {
                char buf[32];
                std::snprintf(buf, sizeof buf, "%.6g", d);
                return buf;
            }
            return "null";
        }
        case Value::Type::String: return "\"" + escape(v.as_string()) + "\"";
        case Value::Type::Array: {
            if (v.items().empty()) return "[]";
            std::string out = "[\n";
            bool first = true;
            for (const auto& it : v.items()) {
                // 数组元素之间空一行，突出每个元素（如 cameras 每台相机）
                if (!first) out += ",\n\n";
                out += pad(indent) + dump_pretty(it, indent + 2);
                first = false;
            }
            return out + "\n" + pad(indent - 2) + "]";
        }
        case Value::Type::Object: {
            if (v.members().empty()) return "{}";
            std::string out = "{\n";
            bool first = true;
            for (const auto& [k, val] : v.members()) {
                if (!first) out += ",\n";
                out += pad(indent) + "\"" + escape(k) + "\": " + dump_pretty(val, indent + 2);
                first = false;
            }
            return out + "\n" + pad(indent - 2) + "}";
        }
    }
    return "null";
}

// ---- 解析 ----
class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}
    Value parse() {
        skip_ws();
        Value v = parse_value();
        skip_ws();
        if (pos_ != s_.size()) throw std::runtime_error("JSON 尾部多余字符");
        return v;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;

    void skip_ws() { while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) pos_++; }
    char peek() { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    char next() { return pos_ < s_.size() ? s_[pos_++] : '\0'; }
    void expect(char c) { if (next() != c) throw std::runtime_error("JSON 语法错误"); }

    Value parse_value() {
        char c = peek();
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == '"') return Value(parse_string());
        if (c == 't') { expect('t'); expect('r'); expect('u'); expect('e'); return Value(true); }
        if (c == 'f') { expect('f'); expect('a'); expect('l'); expect('s'); expect('e'); return Value(false); }
        if (c == 'n') { expect('n'); expect('u'); expect('l'); expect('l'); return Value(nullptr); }
        return parse_number();
    }

    std::string parse_string() {
        expect('"');
        std::string out;
        while (pos_ < s_.size()) {
            char c = next();
            if (c == '"') return out;
            if (c == '\\') {
                char e = next();
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        unsigned code = 0;
                        for (int i = 0; i < 4; i++) {
                            char h = next();
                            code <<= 4;
                            if (h >= '0' && h <= '9') code |= h - '0';
                            else if (h >= 'a' && h <= 'f') code |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') code |= h - 'A' + 10;
                            else throw std::runtime_error("JSON \\u 错误");
                        }
                        // 只支持 BMP
                        if (code < 0x80) out += static_cast<char>(code);
                        else if (code < 0x800) {
                            out += static_cast<char>(0xC0 | (code >> 6));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (code >> 12));
                            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        }
                        break;
                    }
                    default: throw std::runtime_error("JSON 转义错误");
                }
            } else if (static_cast<unsigned char>(c) < 0x20) {
                throw std::runtime_error("JSON 字符串含控制字符");
            } else out += c;
        }
        throw std::runtime_error("JSON 字符串未闭合");
    }

    Value parse_number() {
        size_t start = pos_;
        if (peek() == '-') next();
        while (peek() >= '0' && peek() <= '9') next();
        bool is_double = false;
        if (peek() == '.') { is_double = true; next(); while (peek() >= '0' && peek() <= '9') next(); }
        if (peek() == 'e' || peek() == 'E') {
            is_double = true; next();
            if (peek() == '+' || peek() == '-') next();
            while (peek() >= '0' && peek() <= '9') next();
        }
        if (start == pos_) throw std::runtime_error("JSON 数字错误");
        std::string tok = s_.substr(start, pos_ - start);
        if (is_double) return Value(std::stod(tok));
        return Value(static_cast<int64_t>(std::stoll(tok)));
    }

    Value parse_object() {
        expect('{');
        Value obj = Value::object();
        skip_ws();
        if (peek() == '}') { next(); return obj; }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            expect(':');
            skip_ws();
            obj[key] = parse_value();
            skip_ws();
            char c = next();
            if (c == ',') continue;
            if (c == '}') break;
            throw std::runtime_error("JSON 对象语法错误");
        }
        return obj;
    }

    Value parse_array() {
        expect('[');
        Value arr = Value::array();
        skip_ws();
        if (peek() == ']') { next(); return arr; }
        while (true) {
            skip_ws();
            arr.push_back(parse_value());
            skip_ws();
            char c = next();
            if (c == ',') continue;
            if (c == ']') break;
            throw std::runtime_error("JSON 数组语法错误");
        }
        return arr;
    }
};

inline Value parse(const std::string& s) { return Parser(s).parse(); }

}  // namespace camera::json
