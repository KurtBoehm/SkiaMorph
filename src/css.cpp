// This file is part of https://github.com/KurtBoehm/SkiaMorph.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <any>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/base.h>

#include "ANTLRInputStream.h"
#include "CommonTokenStream.h"
#include "ParserRuleContext.h"

#include "antlr-css3/css3Lexer.h"
#include "antlr-css3/css3Parser.h"
#include "antlr-css3/css3ParserBaseVisitor.h"
#include "css.hpp"

// Extracts `.class { ... }` rules from a CSS3 stylesheet using the ANTLR css3 grammar.
//
// Supported:
//   - Single selector, no combinators
//   - Class selectors only (`.foo`)
//   - `property: value` declarations
//
// Throws on:
//   - Combinators (`.a .b`, `.a > .b`, …)
//   - Non‑class selectors (`#id`, `div`, `*`, …)
struct CSSExtractor : public css3ParserBaseVisitor {
  // `pcss` must outlive this visitor (used by `getText`).
  CSSExtractor(std::string_view pcss) : css{pcss} {}

  std::string_view css;
  std::vector<css::Rule> rules{};

  // `knownRuleset: selectorGroup '{' ws declarationList? '}' ws`
  //
  // Restrictions:
  //   - `selectorGroup` must contain exactly one `selector` (no combinators)
  //   - `selector` must contain exactly:
  //     - `[0]` `SimpleSelectorSequence`
  //     - `[1]` whitespace
  //   - `SimpleSelectorSequence` must contain exactly one `ClassName`
  std::any visitKnownRuleset(css3Parser::KnownRulesetContext* ctx) override {
    const auto& selector_group = *ctx->selectorGroup();
    if (selector_group.children.size() != 1) {
      throw std::runtime_error{"combinators are currently not supported"};
    }

    auto* selector = dynamic_cast<css3Parser::SelectorContext*>(selector_group.children[0]);
    if (!selector) {
      throw std::runtime_error{"selector is not a SelectorContext"};
    }
    if (selector->children.size() != 2) {
      throw std::runtime_error{
        "selector does not have exactly two children (actual selector and whitespace)"};
    }

    auto* simple_selector_sequence =
      dynamic_cast<css3Parser::SimpleSelectorSequenceContext*>(selector->children[0]);
    if (!simple_selector_sequence) {
      throw std::runtime_error{"selector sequence is not a SimpleSelectorSequenceContext"};
    }
    if (simple_selector_sequence->children.size() != 1) {
      throw std::runtime_error{"selector sequence does not have exactly one child"};
    }

    auto* class_name =
      dynamic_cast<css3Parser::ClassNameContext*>(simple_selector_sequence->children[0]);
    if (!class_name) {
      throw std::runtime_error{"only class names are supported"};
    }

    css::Rule& rule =
      rules.emplace_back(css::Selector{.class_name = std::string{getText(*class_name->ident())}});

    css::Rule* prev = currentRule;
    currentRule = &rule;

    if (auto* dl = ctx->declarationList()) {
      visit(dl);
    }

    currentRule = prev;
    return nullptr;
  }

  // `knownDeclaration: property_ ':' ws expr prio?`
  std::any visitKnownDeclaration(css3Parser::KnownDeclarationContext* ctx) override {
    if (!currentRule) {
      throw std::runtime_error{"declaration without a rule is invalid"};
    }

    auto* propCtx = ctx->property_();
    auto* exprCtx = ctx->expr();

    currentRule->decls.emplace_back(propCtx->getText(), exprCtx->getText());
    return nullptr;
  }

private:
  // Exact source slice for `ctx` from original CSS.
  std::string_view getText(const antlr4::ParserRuleContext& ctx) {
    const auto i0 = ctx.getStart()->getStartIndex();
    const auto i1 = ctx.getStop()->getStopIndex() + 1;
    return css.substr(i0, i1 - i0);
  }

  css::Rule* currentRule = nullptr;
};

// Matches iff class names are equal.
bool css::Selector::matches(std::string_view class_name) const {
  return this->class_name == class_name;
}

// First matching rule for `class_name`, or `nullptr`.
const css::Rule* css::Stylesheet::match(std::string_view class_name) const {
  for (const auto& rule : rules) {
    if (rule.selector.matches(class_name)) {
      return &rule;
    }
  }
  return nullptr;
}

// Parse CSS into a `css::Stylesheet`. See `CSSExtractor` for limitations.
css::Stylesheet css::parse(std::string_view css) {
  antlr4::ANTLRInputStream input{css};
  css3Lexer lexer{&input};
  antlr4::CommonTokenStream tokens{&lexer};
  css3Parser parser{&tokens};

  auto* tree = parser.stylesheet();
#ifndef NDEBUG
  fmt::print("tree:\n{}\n", tree->toStringTree(&parser, true));
#endif

  CSSExtractor extractor{css};
  extractor.visit(tree);

#ifndef NDEBUG
  for (const auto& rule : extractor.rules) {
    fmt::print("Selector: {}\n", rule.selector);
    for (const auto& d : rule.decls) {
      fmt::print(" {}: {}\n", d.property, d.value);
    }
  }
#endif

  return Stylesheet{.rules = std::move(extractor.rules)};
}
