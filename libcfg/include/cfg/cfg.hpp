// Copyright (c) 2026 Philipp Orlov
// SPDX-License-Identifier: Apache-2.0

// One typed-parameter model for argv, environment variables and URL query strings. A parameter is declared once
// with its target variable, default, range and choices; every source is validated the same way, so a port of 70000
// is rejected whether it came from --port, GSCOPE_PORT or ?port=.
//
//   cfg::Schema s;
//   uint16_t port;  double interval;  std::string mode;  bool verbose;
//   s.addInt(port, "port", 8080, 1, 65535, "listen port");
//   s.addDouble(interval, "interval", 1.0, 0.1, 3600.0);
//   s.addChoice(mode, "mode", "fast", {"fast", "safe"});
//   s.addBool(verbose, "verbose", false);
//   base::Status st = s.fromArgv(argc, argv);     // "--port 80", "--port=80", "--verbose", "--no-verbose", "port=80"
//   if (!st.ok()) { std::fputs(st.message()...); }
#pragma once

#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "base/status.hpp"

namespace cfg {

class Schema {
public:
    // Integers of any width/signedness: the allowed range is the intersection of [lo, hi] and the type's range.
    template <class T>
    Schema& addInt(T& target, std::string name, T def, T lo = std::numeric_limits<T>::lowest(), T hi = std::numeric_limits<T>::max(),
                   std::string help = {}) {
        static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>, "use addBool for bool");
        target = def;
        Param p;
        p.name = std::move(name);
        p.help = std::move(help);
        p.kind = std::is_signed_v<T> ? Kind::Int : Kind::UInt;
        p.imin = static_cast<std::int64_t>(lo);
        p.imax = static_cast<std::int64_t>(hi);
        if (!std::is_signed_v<T>) {
            p.umin = static_cast<std::uint64_t>(lo);
            p.umax = static_cast<std::uint64_t>(hi);
        }
        p.setInt = [&target](std::int64_t v) { target = static_cast<T>(v); };
        p.setUInt = [&target](std::uint64_t v) { target = static_cast<T>(v); };
        p.defaultText = std::to_string(def);
        params_.push_back(std::move(p));
        return *this;
    }
    Schema& addDouble(double& target, std::string name, double def, double lo, double hi, std::string help = {});
    Schema& addBool(bool& target, std::string name, bool def, std::string help = {});
    Schema& addString(std::string& target, std::string name, std::string def, std::string help = {});
    Schema& addChoice(std::string& target, std::string name, std::string def, std::vector<std::string> choices, std::string help = {});
    // Marks the most recently added parameter as required (it must be set by some source before validate()).
    Schema& required();

    // Sources. Each sets only the parameters it mentions; the first error stops it and names the parameter.
    base::Status fromArgv(int argc, const char* const* argv);                 // argv[0] is skipped
    base::Status fromEnv(std::string_view prefix);                            // PREFIX + NAME upper-cased, '-' -> '_'
    base::Status fromQuery(std::string_view query);                           // a=1&b=two, percent-decoded
    base::Status set(std::string_view name, std::string_view value);          // one parameter, by name
    base::Status validate() const;                                            // required parameters were provided

    std::string help() const;                                                 // one line per parameter with range/default
    // Names given on the command line that no parameter claims (positionals) land here.
    const std::vector<std::string>& positional() const noexcept { return positional_; }

private:
    enum class Kind { Int, UInt, Double, Bool, String, Choice };
    struct Param {
        std::string name, help, defaultText;
        Kind kind = Kind::String;
        bool required = false, seen = false;
        std::int64_t imin = 0, imax = 0;
        std::uint64_t umin = 0, umax = 0;
        double dmin = 0, dmax = 0;
        std::vector<std::string> choices;
        std::function<void(std::int64_t)> setInt;
        std::function<void(std::uint64_t)> setUInt;
        std::function<void(double)> setDouble;
        std::function<void(bool)> setBool;
        std::function<void(std::string_view)> setString;
    };
    Param* find(std::string_view name);
    base::Status assign(Param& p, std::string_view value);

    std::vector<Param> params_;
    std::vector<std::string> positional_;
};

}  // namespace cfg
