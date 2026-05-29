#pragma once

#include <algorithm>
#include <cstdio>
#include <flat_map>
#include <ostream>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalyst {

struct JSON {
    enum class Type : std::uint8_t { Null, Boolean, Number, String, Array, Object };

    Type type = Type::Null;
    bool bool_val = false;
    double num_val = 0.0;
    std::string str_val;
    std::vector<JSON> arr_val;
    std::flat_map<std::string, JSON> obj_val;

    JSON() = default;
    JSON(const JSON &other) = default;
    JSON(JSON &&other) = default;
    JSON &operator=(const JSON &other) = default;
    JSON &operator=(JSON &&other) = default;

    ~JSON() = default;
    explicit JSON(bool b) : type(Type::Boolean), bool_val(b) {
    }
    explicit JSON(double d) : type(Type::Number), num_val(d) {
    }
    explicit JSON(const std::string &s) : type(Type::String), str_val(s) {
    }
    explicit JSON(std::string &&s) : type(Type::String), str_val(std::move(s)) {
    }
    explicit JSON(const char *s) : type(Type::String), str_val(s) {
    }
    explicit JSON(std::string_view s) : type(Type::String), str_val(s) {
    }

    explicit JSON(const std::vector<std::string> &vec) : type(Type::Array), arr_val(vec.begin(), vec.end()) {
    }

    explicit JSON(std::vector<std::string> &&vec) : type(Type::Array), arr_val(vec.begin(), vec.end()) {
    }

    static JSON array();
    static JSON object();

    JSON &operator[](const std::string &key);

    void push_back(const JSON &val);
    void push_back(JSON &&val);

    JSON &operator=(bool b);
    JSON &operator=(double d);
    JSON &operator=(const std::string &s);
    JSON &operator=(std::string &&s);
    JSON &operator=(const char *s);
    JSON &operator=(std::string_view s);
    JSON &operator=(const std::vector<std::string> &vec);
    JSON &operator=(std::vector<std::string> &&vec);
};

} // namespace catalyst
