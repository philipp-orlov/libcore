// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

#include "cfg/cfg.hpp"

#include <charconv>
#include <cstdlib>
#include <cstring>

namespace cfg {

namespace {
base::Status invalid(std::string_view name, const std::string& what) {
    std::string text = "--" + std::string(name) + ": " + what;
    return base::Status(base::Code::InvalidArgument, text);
}

std::string percentDecode(std::string_view in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size() + 0 && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            out += static_cast<char>(std::stoi(std::string(in.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}
}  // namespace

Schema& Schema::addDouble(double& target, std::string name, double def, double lo, double hi, std::string help) {
    target = def;
    Param p;
    p.name = std::move(name);
    p.help = std::move(help);
    p.kind = Kind::Double;
    p.dmin = lo;
    p.dmax = hi;
    p.setDouble = [&target](double v) { target = v; };
    p.defaultText = std::to_string(def);
    params_.push_back(std::move(p));
    return *this;
}

Schema& Schema::addBool(bool& target, std::string name, bool def, std::string help) {
    target = def;
    Param p;
    p.name = std::move(name);
    p.help = std::move(help);
    p.kind = Kind::Bool;
    p.setBool = [&target](bool v) { target = v; };
    p.defaultText = def ? "true" : "false";
    params_.push_back(std::move(p));
    return *this;
}

Schema& Schema::addString(std::string& target, std::string name, std::string def, std::string help) {
    target = def;
    Param p;
    p.name = std::move(name);
    p.help = std::move(help);
    p.kind = Kind::String;
    p.setString = [&target](std::string_view v) { target.assign(v); };
    p.defaultText = std::move(def);
    params_.push_back(std::move(p));
    return *this;
}

Schema& Schema::addChoice(std::string& target, std::string name, std::string def, std::vector<std::string> choices, std::string help) {
    addString(target, std::move(name), std::move(def), std::move(help));
    params_.back().kind = Kind::Choice;
    params_.back().choices = std::move(choices);
    return *this;
}

Schema& Schema::required() {
    if (!params_.empty())
        params_.back().required = true;
    return *this;
}

Schema::Param* Schema::find(std::string_view name) {
    for (Param& p : params_)
        if (p.name == name)
            return &p;
    return nullptr;
}

base::Status Schema::assign(Param& p, std::string_view value) {
    switch (p.kind) {
        case Kind::Int: {
            std::int64_t v = 0;
            const auto r = std::from_chars(value.data(), value.data() + value.size(), v);
            if (value.empty() || r.ec == std::errc::invalid_argument || r.ptr != value.data() + value.size())
                return invalid(p.name, "'" + std::string(value) + "' is not an integer");
            if (r.ec == std::errc::result_out_of_range || v < p.imin || v > p.imax)
                return invalid(p.name, std::string(value) + " is outside " + std::to_string(p.imin) + ".." + std::to_string(p.imax));
            p.setInt(v);
            break;
        }
        case Kind::UInt: {
            std::uint64_t v = 0;
            const auto r = std::from_chars(value.data(), value.data() + value.size(), v);
            if (value.empty() || value[0] == '-' || r.ec == std::errc::invalid_argument || r.ptr != value.data() + value.size())
                return invalid(p.name, "'" + std::string(value) + "' is not a non-negative integer");
            if (r.ec == std::errc::result_out_of_range || v < p.umin || v > p.umax)
                return invalid(p.name, std::string(value) + " is outside " + std::to_string(p.umin) + ".." + std::to_string(p.umax));
            p.setUInt(v);
            break;
        }
        case Kind::Double: {
            // from_chars for double is missing on older libstdc++ (GCC < 11): strtod on a bounded copy, C locale only
            // matters for the decimal point, which strtod takes from LC_NUMERIC -- so reject anything that is not
            // plain [-+]digits[.digits][e[-+]digits] first.
            std::string text(value);
            bool ok = !text.empty();
            for (char c : text)
                if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E'))
                    ok = false;
            char* end = nullptr;
            const double v = ok ? std::strtod(text.c_str(), &end) : 0;
            if (!ok || end != text.c_str() + text.size() || v != v)
                return invalid(p.name, "'" + text + "' is not a number");
            if (v < p.dmin || v > p.dmax)
                return invalid(p.name, text + " is outside " + std::to_string(p.dmin) + ".." + std::to_string(p.dmax));
            p.setDouble(v);
            break;
        }
        case Kind::Bool: {
            if (value == "1" || value == "true" || value == "yes" || value == "on" || value.empty())
                p.setBool(true);
            else if (value == "0" || value == "false" || value == "no" || value == "off")
                p.setBool(false);
            else
                return invalid(p.name, "'" + std::string(value) + "' is not a boolean");
            break;
        }
        case Kind::Choice: {
            bool found = false;
            for (const std::string& c : p.choices)
                if (c == value)
                    found = true;
            if (!found) {
                std::string list;
                for (const std::string& c : p.choices)
                    list += (list.empty() ? "" : "|") + c;
                return invalid(p.name, "'" + std::string(value) + "' is not one of " + list);
            }
            p.setString(value);
            break;
        }
        case Kind::String:
            p.setString(value);
            break;
    }
    p.seen = true;
    return {};
}

base::Status Schema::set(std::string_view name, std::string_view value) {
    Param* p = find(name);
    if (!p)
        return base::Status(base::Code::InvalidArgument, "unknown option: " + std::string(name));
    return assign(*p, value);
}

base::Status Schema::fromArgv(int argc, const char* const* argv) {
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg.size() > 2 && arg.substr(0, 2) == "--") {
            arg.remove_prefix(2);
            std::string_view name = arg, value;
            bool hasValue = false;
            if (const std::size_t eq = arg.find('='); eq != std::string_view::npos) {
                name = arg.substr(0, eq);
                value = arg.substr(eq + 1);
                hasValue = true;
            }
            Param* p = find(name);
            if (!p && name.substr(0, 3) == "no-" && !hasValue) {
                if (Param* b = find(name.substr(3)); b && b->kind == Kind::Bool) {
                    if (auto st = assign(*b, "false"); !st.ok()) return st;
                    continue;
                }
            }
            if (!p)
                return base::Status(base::Code::InvalidArgument, "unknown option: --" + std::string(name));
            if (!hasValue) {
                if (p->kind == Kind::Bool) {
                    if (auto st = assign(*p, "true"); !st.ok()) return st;
                    continue;
                }
                if (i + 1 >= argc)
                    return invalid(p->name, "needs a value");
                value = argv[++i];
            }
            if (auto st = assign(*p, value); !st.ok())
                return st;
        } else if (const std::size_t eq = arg.find('='); eq != std::string_view::npos && eq > 0 && find(arg.substr(0, eq))) {
            if (auto st = assign(*find(arg.substr(0, eq)), arg.substr(eq + 1)); !st.ok())
                return st;
        } else {
            positional_.emplace_back(arg);
        }
    }
    return {};
}

base::Status Schema::fromEnv(std::string_view prefix) {
    for (Param& p : params_) {
        std::string var(prefix);
        for (char c : p.name)
            var += c == '-' ? '_' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (const char* v = std::getenv(var.c_str()))
            if (auto st = assign(p, v); !st.ok())
                return base::Status(st.code(), var + ": " + std::string(st.message()).substr(std::string(st.message()).find(": ") + 2));
    }
    return {};
}

base::Status Schema::fromQuery(std::string_view query) {
    while (!query.empty()) {
        const std::size_t amp = query.find('&');
        const std::string_view pair = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view{} : query.substr(amp + 1);
        if (pair.empty())
            continue;
        const std::size_t eq = pair.find('=');
        const std::string name = percentDecode(pair.substr(0, eq));
        const std::string value = eq == std::string_view::npos ? std::string() : percentDecode(pair.substr(eq + 1));
        Param* p = find(name);
        if (!p)
            continue;   // unknown query parameters are ignored: URLs carry things that are not ours
        if (auto st = assign(*p, value); !st.ok())
            return st;
    }
    return {};
}

base::Status Schema::validate() const {
    for (const Param& p : params_)
        if (p.required && !p.seen)
            return base::Status(base::Code::InvalidArgument, "missing required option: --" + p.name);
    return {};
}

std::string Schema::help() const {
    std::string out;
    for (const Param& p : params_) {
        out += "  --" + p.name;
        switch (p.kind) {
            case Kind::Int: out += " <" + std::to_string(p.imin) + ".." + std::to_string(p.imax) + ">"; break;
            case Kind::UInt: out += " <" + std::to_string(p.umin) + ".." + std::to_string(p.umax) + ">"; break;
            case Kind::Double: out += " <" + std::to_string(p.dmin) + ".." + std::to_string(p.dmax) + ">"; break;
            case Kind::Choice: {
                out += " <";
                for (std::size_t i = 0; i < p.choices.size(); ++i)
                    out += (i ? "|" : "") + p.choices[i];
                out += ">";
                break;
            }
            case Kind::String: out += " <text>"; break;
            case Kind::Bool: break;
        }
        out += p.required ? "  (required)" : "  (default " + p.defaultText + ")";
        if (!p.help.empty())
            out += "  " + p.help;
        out += "\n";
    }
    return out;
}

}  // namespace cfg
