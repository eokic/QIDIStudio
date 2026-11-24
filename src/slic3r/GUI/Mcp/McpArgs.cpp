#include "McpArgs.hpp"

#include <boost/algorithm/string/predicate.hpp>

#include <cmath>

namespace Slic3r { namespace GUI { namespace Mcp {

using json = nlohmann::json;

namespace {

bool given(const json &args, const std::string &name) { return args.is_object() && args.contains(name) && !args[name].is_null(); }

json described(json schema, const std::string &description)
{
    if (!description.empty()) schema["description"] = description;
    return schema;
}

// "'plate' is required: 1-based plate".
ToolError missing(const Arg &a)
{
    const json d = a.schema.value("description", json());
    return ToolError("'" + a.name + "' is required" + (d.is_string() ? ": " + d.get<std::string>() : std::string()));
}

// "from 1 to 16", "of 1 or more", "" (any int).
std::string bounds_text(int min, int max)
{
    if (min == INT_MIN && max == INT_MAX) return "";
    if (max == INT_MAX) return " of " + std::to_string(min) + " or more";
    return " from " + std::to_string(min) + " to " + std::to_string(max);
}

std::string range_text(int min, int max) { return "a whole number" + bounds_text(min, max); }

bool whole_in(const json &v, int min, int max)
{
    return v.is_number() && v.get<double>() == std::trunc(v.get<double>()) && v.get<double>() >= min && v.get<double>() <= max;
}

int in_range(const json &v, const std::string &name, int min, int max)
{
    if (!whole_in(v, min, max)) throw ToolError("'" + name + "' must be " + range_text(min, max));
    return v.get<int>();
}

json int_schema(int min, int max)
{
    json s = {{"type", "integer"}};
    if (min != INT_MIN) s["minimum"] = min;
    if (max != INT_MAX) s["maximum"] = max;
    return s;
}

} // namespace

json input_schema(std::initializer_list<std::reference_wrapper<const Arg>> args)
{
    json props = json::object(), required = json::array();
    for (const Arg &a : args) {
        props[a.name] = a.schema;
        if (a.required) required.push_back(a.name);
    }
    json s = {{"type", "object"}, {"properties", props}};
    if (!required.empty()) s["required"] = required;
    return s;
}

IntArg::IntArg(std::string name, std::string description, int min, int max, std::optional<int> def)
    : Arg{std::move(name), described(int_schema(min, max), description), !def.has_value()}, min(min), max(max), def(def)
{
    if (def && *def >= min && *def <= max) schema["default"] = *def;
}

int IntArg::read(const json &args) const
{
    if (!given(args, name)) {
        if (def) return *def;
        throw missing(*this);
    }
    return in_range(args[name], name, min, max);
}

IntListArg::IntListArg(std::string name, std::string description, int min, int max)
    : Arg{std::move(name), described({{"type", "array"}, {"items", int_schema(min, max)}}, description)}, min(min), max(max)
{}

std::vector<int> IntListArg::read(const json &args) const
{
    std::vector<int> out;
    if (!given(args, name)) return out;
    bool ok = args[name].is_array();
    for (size_t i = 0; ok && i < args[name].size(); ++i) ok = whole_in(args[name][i], min, max);
    if (!ok) throw ToolError("'" + name + "' must be a list of whole numbers" + bounds_text(min, max));
    for (const json &v : args[name]) out.push_back(v.get<int>());
    return out;
}

VecArg::VecArg(std::string name, std::string description, size_t min_size, bool number_ok)
    : Arg{std::move(name), {}}, min_size(min_size), number_ok(number_ok)
{
    json list = {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", min_size}, {"maxItems", 3}};
    schema    = described(number_ok ? json{{"anyOf", {{{"type", "number"}}, list}}} : list, description);
}

std::optional<std::array<double, 3>> VecArg::read(const json &args) const
{
    if (!given(args, name)) return std::nullopt;
    const json &v = args[name];
    if (number_ok && v.is_number()) return std::array<double, 3>{v.get<double>(), v.get<double>(), v.get<double>()};
    const std::string form = min_size == 3 ? "3 numbers [x,y,z]" : "2 or 3 numbers [x,y] or [x,y,z]";
    bool ok = v.is_array() && v.size() >= min_size && v.size() <= 3;
    for (size_t i = 0; ok && i < v.size(); ++i) ok = v[i].is_number();
    if (!ok) throw ToolError("'" + name + "' must be " + (number_ok ? "a number or " : "") + form);
    return std::array<double, 3>{v[0].get<double>(), v[1].get<double>(), v.size() == 3 ? v[2].get<double>() : 0.0};
}

EnumArg::EnumArg(std::string name, std::string description, std::vector<std::string> values, std::optional<std::string> def)
    : Arg{std::move(name), described({{"type", "string"}, {"enum", values}}, description), !def.has_value()}
    , values(std::move(values))
    , def(std::move(def))
{
    if (this->def) schema["default"] = *this->def;
}

std::string EnumArg::read(const json &args) const
{
    if (!given(args, name)) {
        if (def) return *def;
        throw missing(*this);
    }
    const json &v = args[name];
    if (v.is_string())
        for (const std::string &s : values)
            if (v.get<std::string>() == s) return s;
    std::string list;
    for (const std::string &s : values) list += (list.empty() ? "" : ", ") + s;
    throw ToolError("'" + name + "' must be one of " + list);
}

StringArg::StringArg(std::string name, std::string description, bool required, std::string suffix)
    : Arg{std::move(name), described({{"type", "string"}}, description), required}, suffix(std::move(suffix))
{}

std::string StringArg::read(const json &args) const
{
    std::string s;
    if (given(args, name)) {
        if (!args[name].is_string()) throw ToolError("'" + name + "' must be a string");
        s = args[name].get<std::string>();
    }
    if (s.empty()) {
        if (required) throw missing(*this);
        return s;
    }
    if (!suffix.empty() && !boost::iends_with(s, suffix)) throw ToolError("'" + name + "' must end in " + suffix);
    return s;
}

StringListArg::StringListArg(std::string name, std::string description)
    : Arg{std::move(name), described({{"type", "array"}, {"items", {{"type", "string"}}}}, description)}
{}

std::vector<std::string> StringListArg::read(const json &args) const
{
    std::vector<std::string> out;
    if (!given(args, name)) return out;
    bool ok = args[name].is_array();
    for (size_t i = 0; ok && i < args[name].size(); ++i) ok = args[name][i].is_string();
    if (!ok) throw ToolError("'" + name + "' must be a list of strings");
    for (const json &v : args[name]) out.push_back(v.get<std::string>());
    return out;
}

BoolArg::BoolArg(std::string name, std::string description, std::optional<bool> def, bool required)
    : Arg{std::move(name), described({{"type", "boolean"}}, description), required}, def(def)
{
    if (def) schema["default"] = *def;
}

std::optional<bool> BoolArg::read_if_given(const json &args) const
{
    if (!given(args, name)) return std::nullopt;
    if (!args[name].is_boolean()) throw ToolError("'" + name + "' must be true or false");
    return args[name].get<bool>();
}

bool BoolArg::read(const json &args) const
{
    if (const std::optional<bool> v = read_if_given(args)) return *v;
    if (def) return *def;
    throw missing(*this);
}

int arg_int(const json &args, const char *key, int def) { return given(args, key) ? in_range(args[key], key, INT_MIN, INT_MAX) : def; }

}}} // namespace Slic3r::GUI::Mcp
