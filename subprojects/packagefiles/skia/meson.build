project(
  'skia',
  'cpp',
  version: '0.0.1',
  default_options: ['cpp_std=c++20'],
)

fs = import('fs')
py = import('python')

gn_exe = find_program('gn', required: true)
ninja_exe = find_program('ninja', required: true)
py_exe = py.find_installation('python3')

# GN/Ninja build output directory for Skia
build_dir = meson.current_build_dir() / 'out'

# Base GN args (see Skia GN docs)
gn_args = []

# Map Meson buildtype → Skia is_official_build
if get_option('buildtype').startswith('debug')
  gn_args += ['is_official_build=false']
else
  gn_args += ['is_official_build=true']
endif

# Target OS for GN
if host_machine.system() == 'linux'
  gn_args += ['target_os="linux"']
elif host_machine.system() == 'darwin'
  gn_args += ['target_os="mac"']
elif host_machine.system() == 'windows'
  gn_args += ['target_os="win"']
endif

# Target CPU for GN
if host_machine.cpu_family().startswith('x86')
  gn_args += ['target_cpu="x64"']
elif host_machine.cpu_family() == 'arm'
  gn_args += ['target_cpu="arm"']
elif host_machine.cpu_family() == 'aarch64'
  gn_args += ['target_cpu="arm64"']
endif

# Sync Skia third-party deps
message('tools/git-sync-deps')
run_command(py_exe, 'tools/git-sync-deps', check: true)

# Configure GN build
message('gn gen')
run_command(gn_exe, 'gen', build_dir, '--args=' + ' '.join(gn_args), check: true)

# Build core Skia and SVG-related targets
message('ninja')
ninja_res = run_command(
  ninja_exe,
  '-C', build_dir,
  'skia',
  'modules/svg',
  check: true,
)

# Library names produced by Skia (per platform)
if host_machine.system() == 'windows'
  libname = 'skia.lib'
  svgname = 'svg.lib'
  skshapername = 'skshaper.lib'
  skunicode_core = 'skunicode_core.lib'
  skunicode_icu = 'skunicode_icu.lib'
else
  libname = 'libskia.a'
  svgname = 'libsvg.a'
  skshapername = 'libskshaper.a'
  skunicode_core = 'libskunicode_core.a'
  skunicode_icu = 'libskunicode_icu.a'
endif

# Verify Skia outputs exist
lib_path = build_dir / libname
if not fs.exists(lib_path)
  error(f'Skia library not found at @lib_path@')
endif
svg_path = build_dir / svgname
if not fs.exists(svg_path)
  error(f'Skia SVG library not found at @svg_path@')
endif
skshaper_path = build_dir / skshapername
if not fs.exists(skshaper_path)
  error(f'SkShaper not found at @skshaper_path@')
endif
skunicode_core_path = build_dir / skunicode_core
if not fs.exists(skunicode_core_path)
  error(f'SkUnicode Core not found at @skunicode_core_path@')
endif
skunicode_icu_path = build_dir / skunicode_icu
if not fs.exists(skunicode_icu_path)
  error(f'SkUnicode ICU not found at @skunicode_icu_path@')
endif

inc_dir = include_directories('.')

# Skia SVG / text dependencies
expat_dep = dependency('expat', required: true)
freetype_dep = dependency('freetype2', required: true)
harfbuzz_dep = dependency('harfbuzz', required: true)
icu_dep = dependency('icu-uc', required: true)
jpeg_dep = dependency('libjpeg', required: true)
png_dep = dependency('libpng', required: true)
webp_dep = [
  dependency('libwebp', required: true),
  dependency('libwebpdemux', required: true),
]
zlib_dep = dependency('zlib', required: true)

# Core Skia link info
skia_dep = declare_dependency(
  include_directories: inc_dir,
  link_args: lib_path,
)

# SVG / shaping / Unicode link info and transitive deps
skia_svg_dep = declare_dependency(
  dependencies: [
    expat_dep,
    freetype_dep,
    harfbuzz_dep,
    icu_dep,
    jpeg_dep,
    png_dep,
    webp_dep,
    zlib_dep,
  ],
  include_directories: inc_dir,
  link_args: [svg_path, skshaper_path, skunicode_core_path, skunicode_icu_path],
)
