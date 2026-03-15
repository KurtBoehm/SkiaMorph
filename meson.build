# This file is part of https://github.com/KurtBoehm/SkiaMorph.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

project(
  'SkiaMorph',
  'cpp',
  default_options: ['cpp_std=c++23', 'b_ndebug=if-release', 'b_lto=true'],
)

antlr4_dep = dependency('antlr4-runtime', method: 'cmake', modules: ['antlr4_static'])
fmt_dep = dependency('fmt')
omp_dep = dependency('OpenMP')
skia_sub = subproject('skia')
skia_dep = skia_sub.get_variable('skia_dep')
skia_svg_dep = skia_sub.get_variable('skia_svg_dep')

args = []

compiler = meson.get_compiler('cpp')
compiler_id = compiler.get_id()
is_gcc_compatible = ['gcc', 'clang'].contains(compiler_id)
if is_gcc_compatible and compiler.compiles('int dummy;', name: 'Check -march=native', args: ['-march=native'])
  args += ['-march=native']
endif

antlr_lib = static_library(
  'antlr-css3',
  [
    'src/css.cpp',
    'src/antlr-css3/css3Lexer.cpp',
    'src/antlr-css3/css3ParserBaseListener.cpp',
    'src/antlr-css3/css3Parser.cpp',
    'src/antlr-css3/css3ParserListener.cpp',
  ],
  dependencies: [antlr4_dep, fmt_dep],
  cpp_args: args,
)

executable(
  'SkiaMorph',
  ['src/main.cpp', 'src/svg-dom.cpp'],
  link_with: [antlr_lib],
  dependencies: [fmt_dep, omp_dep, skia_dep, skia_svg_dep],
  override_options: ['cpp_rtti=false'],
  cpp_args: args,
)
