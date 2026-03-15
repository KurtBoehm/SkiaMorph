// This file is part of https://github.com/KurtBoehm/SkiaMorph.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef SRC_CSS_HPP
#define SRC_CSS_HPP

#include <string>
#include <string_view>
#include <vector>

#include <fmt/base.h>
#include <fmt/ranges.h>

namespace css {
/** A single `property: value` CSS declaration. */
struct Declaration {
  std::string property;
  std::string value;
};

/** A selector matching exactly one `.class-name`. */
struct Selector {
  std::string class_name;

  /** Return true if this selector matches the given class name. */
  bool matches(std::string_view class_name) const;
};

/** A CSS rule: a selector plus its declarations. */
struct Rule {
  Selector selector;
  std::vector<Declaration> decls{};
};

/** A stylesheet: a list of rules, searched linearly. */
struct Stylesheet {
  std::vector<Rule> rules{};

  /** Return the first rule matching `class_name`, or nullptr if none. */
  const Rule* match(std::string_view class_name) const;
};

/** Parse a minimal CSS subset (class selectors + declarations). */
Stylesheet parse(std::string_view css);
} // namespace css

template<>
struct fmt::formatter<css::Selector> {
  constexpr const char* parse(fmt::parse_context<char>& ctx) {
    return ctx.begin();
  }
  constexpr auto format(const css::Selector& value, fmt::format_context& ctx) const {
    return fmt::format_to(ctx.out(), "Selector{{.class_name={}}}", value.class_name);
  }
};

template<>
struct fmt::formatter<css::Declaration> {
  constexpr const char* parse(fmt::parse_context<char>& ctx) {
    return ctx.begin();
  }
  constexpr auto format(const css::Declaration& decl, fmt::format_context& ctx) const {
    return fmt::format_to(ctx.out(), "{}={}", decl.property, decl.value);
  }
};

template<>
struct fmt::formatter<css::Rule> {
  constexpr const char* parse(fmt::parse_context<char>& ctx) {
    return ctx.begin();
  }
  constexpr auto format(const css::Rule& rule, fmt::format_context& ctx) const {
    return fmt::format_to(ctx.out(), "Rule{{.selector={}, .decls={}}}", rule.selector, rule.decls);
  }
};

#endif // SRC_CSS_HPP
