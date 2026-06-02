#include "cob/json.hpp"
using catalyst::JSON;

JSON JSON::array() {
    JSON j;
    j.type = Type::Array;
    return j;
}

JSON JSON::object() {
    JSON j;
    j.type = Type::Object;
    return j;
}

JSON &JSON::operator[](const std::string &key) {
    if (type != Type::Object) {
        type = Type::Object;
        arr_val.clear();
        str_val.clear();
        obj_val.clear();
    }
    return obj_val[key];
}

void JSON::push_back(const JSON &val) {
    if (type != Type::Array) {
        type = Type::Array;
        obj_val.clear();
        str_val.clear();
        arr_val.clear();
    }
    arr_val.push_back(val);
}

void JSON::push_back(JSON &&val) {
    if (type != Type::Array) {
        type = Type::Array;
        obj_val.clear();
        str_val.clear();
        arr_val.clear();
    }
    arr_val.push_back(std::move(val));
}

JSON &JSON::operator=(bool b) {
    type = Type::Boolean;
    bool_val = b;
    arr_val.clear();
    str_val.clear();
    obj_val.clear();
    return *this;
}

JSON &JSON::operator=(double d) {
    type = Type::Number;
    num_val = d;
    arr_val.clear();
    str_val.clear();
    obj_val.clear();
    return *this;
}

JSON &JSON::operator=(const std::string &s) {
    type = Type::String;
    str_val = s;
    arr_val.clear();
    obj_val.clear();
    return *this;
}

JSON &JSON::operator=(std::string &&s) {
    type = Type::String;
    str_val = std::move(s);
    arr_val.clear();
    obj_val.clear();
    return *this;
}
JSON &JSON::operator=(const char *s) {
    type = Type::String;
    str_val = s;
    arr_val.clear();
    obj_val.clear();
    return *this;
}
JSON &JSON::operator=(std::string_view s) {
    type = Type::String;
    str_val = s;
    arr_val.clear();
    obj_val.clear();
    return *this;
}
JSON &JSON::operator=(const std::vector<std::string> &vec) {
    type = Type::Array;
    obj_val.clear();
    str_val.clear();
    arr_val.clear();
    arr_val.reserve(vec.size());
    for (const auto &s : vec) {
        arr_val.emplace_back(s);
    }
    return *this;
}
JSON &JSON::operator=(std::vector<std::string> &&vec) {
    type = Type::Array;
    obj_val.clear();
    str_val.clear();
    arr_val.clear();
    arr_val.reserve(vec.size());
    for (auto &&s : vec) {
        arr_val.emplace_back(std::move(s));
    }
    return *this;
}
