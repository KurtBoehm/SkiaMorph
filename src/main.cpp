// This file is part of https://github.com/KurtBoehm/SkiaMorph.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

// -----------------------------------------------------------------------------
// SVG supersampled rasterizer
//
// Renders an SVG into a PNG by analytically sampling paths / images in SVG
// (no Skia raster pipeline). Supports:
//
//   - <path> with solid-color fill + opacity/fill-opacity
//   - <g clip-path="..."> clipping groups
//   - <image> with data:image/{png,webp};base64, href
//   - <use href="#id"> references
//
// The SVG is evaluated in its viewBox coordinate system and mapped to an
// output raster of size width×height, with optional supersampling.
// -----------------------------------------------------------------------------

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <fmt/base.h>
#include <fmt/ranges.h>
#include <fmt/std.h>

#include "include/core/SkAlphaType.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkColorType.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkPoint.h"
#include "include/core/SkRect.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"
#include "modules/svg/include/SkSVGClipPath.h"
#include "modules/svg/include/SkSVGContainer.h"
#include "modules/svg/include/SkSVGG.h"
#include "modules/svg/include/SkSVGImage.h"
#include "modules/svg/include/SkSVGNode.h"
#include "modules/svg/include/SkSVGPath.h"
#include "modules/svg/include/SkSVGTypes.h"
#include "modules/svg/include/SkSVGUse.h"

#include "svg-dom.hpp"

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using uz = std::size_t;
using iz = std::make_signed_t<uz>;
inline constexpr SkScalar inv255 = SkScalar{1} / SkScalar{255};

// std::visit helper
template<typename... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};
template<typename... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

// Tag type for “must be initialized” members.
// Any accidental use in a constexpr context fails to compile.
inline constexpr struct {
  template<typename T>
  constexpr operator T() const; // NOLINT
} init_required{};

// -----------------------------------------------------------------------------
// DOM extraction structures
// -----------------------------------------------------------------------------

struct ClipPathGeom {
  SkPath path{}; // clip path in root coordinates
  SkRect bounds{}; // clip bounds in root coordinates
  const SkSVGClipPath* node{};
};

using ClipPaths = std::unordered_map<std::string_view, ClipPathGeom>;
using IdMap = std::unordered_map<std::string_view, const SkSVGNode*>;

struct Defs {
  ClipPaths clip_paths{};
  IdMap id_map{};
};

struct ClipMask {
  SkPath path;
  SkRect bounds;
};

struct Path {
  SkPath path = init_required; // geometry in root coordinates
  SkRect bounds = init_required; // bounds in root coordinates
  const SkSVGPath* node = init_required;
  std::vector<ClipMask> clips = init_required;

  // Resolved fill and effective opacity in [0,1]
  SkColor fill_color = init_required;
  SkScalar fill_opacity = init_required;

  // Premultiplied components (w.r.t fill_opacity)
  SkScalar premul_r = SkColorGetR(fill_color) * inv255 * fill_opacity;
  SkScalar premul_g = SkColorGetG(fill_color) * inv255 * fill_opacity;
  SkScalar premul_b = SkColorGetB(fill_color) * inv255 * fill_opacity;
};

struct Image {
  SkRect bounds = init_required; // geometry in root coordinates
  const SkSVGImage* node = init_required;
  std::vector<ClipMask> clips = init_required;

  // Decoded image and sampling transform
  sk_sp<SkImage> decoded; // null if decode failed/unsupported
  SkMatrix root_to_image{}; // root SVG -> image pixels
  std::size_t img_width = 0;
  std::size_t img_height = 0;

  // Effective opacity in [0,1] after inheritance
  SkScalar opacity = init_required;

  // Cached pixel buffer (premul N32) and row bytes; may be empty if readPixels failed
  std::vector<u32> pixels{};
  size_t row_bytes = 0;
};

using NodeInfo = std::variant<Path, Image>;

struct Tile {
  std::vector<std::size_t> node_indices; // indices into nodes[]
};
struct TileParams {
  SkScalar xoff, yoff;
  SkScalar xfactor, yfactor;
};

inline constexpr std::size_t tiles_x = 128;
inline constexpr std::size_t tiles_y = 128;

struct Rgba {
  SkScalar r;
  SkScalar g;
  SkScalar b;
  SkScalar a;
};

// -----------------------------------------------------------------------------
// Property helpers
// -----------------------------------------------------------------------------

template<typename T, bool tInheritable>
inline std::optional<T> get(const SkSVGProperty<T, tInheritable>& prop) {
  return prop.isValue() ? std::make_optional(*prop) : std::nullopt;
}

// Extract node id if present.
std::optional<std::string_view> get_id(const SkSVGNode& node) {
  const auto& id = node.getId();
  if (id.size() == 0) {
    return std::nullopt;
  }
  return std::string_view{id.data(), id.size()};
}

// Base64 decode for data URLs (no whitespace, padded or unpadded).
std::vector<u8> base64_decode(std::string_view input) {
  constexpr auto table = [] {
    std::array<u8, 256> t{};
    t.fill(0x80);
    auto set = [&](char c, u8 v) { t[u8(c)] = v; };
    for (std::size_t i = 0; i < 26; ++i) {
      set(char('A' + i), u8(i));
      set(char('a' + i), u8(26 + i));
    }
    for (std::size_t i = 0; i < 10; ++i) {
      set(char('0' + i), u8(52 + i));
    }
    set('+', 62);
    set('/', 63);
    return t;
  }();

  std::vector<u8> out;
  out.reserve(input.size() * 3 / 4);

  u32 acc = 0;
  int bits = 0;

  for (char c : input) {
    if (c == '=') {
      break;
    }
    const u8 d = table[u8(c)];
    if (d == 0x80) {
      continue; // skip invalid chars
    }
    acc = (acc << 6) | d;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(u8((acc >> bits) & 0xFF));
    }
  }
  return out;
}

// -----------------------------------------------------------------------------
// Style resolution
// -----------------------------------------------------------------------------

// Node-local opacity (no inheritance), defaults to 1.
SkScalar resolve_node_opacity(const SkSVGNode& node) {
  SkScalar out = 1;
  if (auto opacity = get(node.getOpacity())) {
    out *= *opacity;
  }
  return out;
}

// Resolve solid fill color and effective opacity chain (fill-opacity * opacity * parent_opacity).
std::pair<SkColor, SkScalar> resolve_fill_and_opacity(const SkSVGNode& node,
                                                      SkScalar parent_opacity) {
  SkColor color = SK_ColorGRAY;
  SkScalar opacity = parent_opacity;

  if (auto fill_opacity = get(node.getFillOpacity())) {
    opacity *= *fill_opacity;
  }
  if (auto node_opacity = get(node.getOpacity())) {
    opacity *= *node_opacity;
  }

  if (auto fill = get(node.getFill())) {
    if (fill->type() == SkSVGPaint::Type::kColor) {
      color = fill->color().color();
    } else if (fill->type() == SkSVGPaint::Type::kNone) {
      opacity = 0;
    }
  }

  return {color, opacity};
}

// -----------------------------------------------------------------------------
// Geometry extraction
// -----------------------------------------------------------------------------

// Append all drawable child geometry of node (used for clip paths).
void append_child_geometry_to_path(const SkSVGNode& node, const SkMatrix& parent_transform,
                                   SkPathBuilder& out_path) {
  switch (node.tag()) {
    case SkSVGTag::kPath: {
      const auto& path_node = static_cast<const SkSVGPath&>(node);
      out_path.addPath(path_node.getPath().makeTransform(parent_transform));
      break;
    }

    // Recurse into containers
    case SkSVGTag::kG:
    case SkSVGTag::kSvg:
    case SkSVGTag::kDefs: {
      const auto& container = static_cast<const SkSVGContainer&>(node);
      const auto child_trans = container.getTransform() * parent_transform;
      container.forEachChild([&](const SkSVGNode* child) {
        append_child_geometry_to_path(*child, child_trans, out_path);
      });
      break;
    }

    default: break;
  }
}

// First pass: collect clip-path geometries and id → node map.
void collect_defs(const SkSVGNode& node, const SkMatrix& parent_transform, Defs& defs) {
  if (auto id_sv = get_id(node)) {
    defs.id_map.emplace(*id_sv, &node);
  }

  if (node.tag() == SkSVGTag::kClipPath) {
    const auto& clipPath = static_cast<const SkSVGClipPath&>(node);
    const auto child_trans = clipPath.getTransform() * parent_transform;
    if (auto id_sv = get_id(node)) {
      SkPathBuilder builder{};
      clipPath.forEachChild([&](const SkSVGNode* child) {
        append_child_geometry_to_path(*child, child_trans, builder);
      });
      SkPath path = builder.detach();
      const SkRect b = path.getBounds();
      defs.clip_paths.emplace(
        *id_sv, ClipPathGeom{.path = std::move(path), .bounds = b, .node = &clipPath});
    }
  }

  if (node.tag() == SkSVGTag::kDefs || node.tag() == SkSVGTag::kG || node.tag() == SkSVGTag::kSvg) {
    const auto& container = static_cast<const SkSVGContainer&>(node);
    const auto child_trans = container.getTransform() * parent_transform;
    container.forEachChild(
      [&](const SkSVGNode* child) { collect_defs(*child, child_trans, defs); });
  }
}

// -----------------------------------------------------------------------------
// Scene flattening (paint order)
// -----------------------------------------------------------------------------

struct Collector {
  std::vector<NodeInfo>& out_nodes;
  std::vector<ClipMask>& clips;
  const Defs& defs;
  SkRect viewbox;

  void collect_group_children(const SkSVGContainer& container, const SkMatrix& parent_transform,
                              SkScalar parent_opacity) {
    container.forEachChild([&](const SkSVGNode* child) {
      collect_paths_in_paint_order(*child, parent_transform, parent_opacity);
    });
  }

  // Flatten SVG DOM into a back-to-front list of drawable nodes (paths, images, clip groups).
  void collect_paths_in_paint_order(const SkSVGNode& node, const SkMatrix& parent_transform,
                                    SkScalar parent_opacity) {
    switch (node.tag()) {
      case SkSVGTag::kPath: {
        const auto& path_node = static_cast<const SkSVGPath&>(node);
        SkPath path =
          path_node.getPath().makeTransform(path_node.getTransform() * parent_transform);
        const SkRect bounds = path.getBounds();
        if (bounds.isEmpty() || !bounds.intersects(viewbox)) {
          return;
        }

        const auto [fill_color, fill_opacity] = resolve_fill_and_opacity(path_node, parent_opacity);
        if (fill_opacity <= 0) {
          return;
        }

        out_nodes.push_back(Path{
          .path = std::move(path),
          .bounds = bounds,
          .node = &path_node,
          .clips = clips,
          .fill_color = fill_color,
          .fill_opacity = fill_opacity,
        });
        break;
      }

      case SkSVGTag::kImage: {
        const auto& img = static_cast<const SkSVGImage&>(node);

        SkRect local_bounds = SkRect::MakeXYWH(img.getX().value(), img.getY().value(),
                                               img.getWidth().value(), img.getHeight().value());
        const SkMatrix m = img.getTransform() * parent_transform;
        SkRect bounds{};
        m.mapRect(&bounds, local_bounds);
        if (bounds.isEmpty() || !bounds.intersects(viewbox)) {
          return;
        }

        const SkScalar image_opacity = parent_opacity * resolve_node_opacity(img);
        if (image_opacity <= 0) {
          return;
        }

        Image out_img{
          .bounds = bounds,
          .node = &img,
          .clips = clips,
          .decoded = nullptr,
          .root_to_image = SkMatrix::I(),
          .img_width = 0,
          .img_height = 0,
          .opacity = image_opacity,
        };

        // Decode data URL for PNG/WEBP images.
        const SkString& href = img.getHref().iri();
        std::string_view href_sv{href.c_str(), std::size_t(href.size())};

        constexpr std::string_view kPngPrefix = "data:image/png;base64,";
        constexpr std::string_view kWebpPrefix = "data:image/webp;base64,";

        std::string_view b64_part{};
        if (href_sv.starts_with(kPngPrefix)) {
          b64_part = href_sv.substr(kPngPrefix.size());
        } else if (href_sv.starts_with(kWebpPrefix)) {
          b64_part = href_sv.substr(kWebpPrefix.size());
        }

        if (!b64_part.empty()) {
          std::vector<u8> decoded_bytes = base64_decode(b64_part);
          if (!decoded_bytes.empty()) {
            sk_sp<SkData> img_data =
              SkData::MakeWithCopy(decoded_bytes.data(), decoded_bytes.size());
            if (img_data) {
              if (sk_sp<SkImage> skimg = SkImages::DeferredFromEncodedData(std::move(img_data))) {
                out_img.decoded = skimg;
                out_img.img_width = static_cast<std::size_t>(skimg->width());
                out_img.img_height = static_cast<std::size_t>(skimg->height());

                // root → element: inverse(parent_transform * img.getTransform())
                SkMatrix elem_from_root;
                if ((parent_transform * img.getTransform()).invert(&elem_from_root)) {
                  const SkScalar sx = img.getWidth().value() != 0
                                        ? SkScalar(out_img.img_width) / img.getWidth().value()
                                        : SkScalar{0};
                  const SkScalar sy = img.getHeight().value() != 0
                                        ? SkScalar(out_img.img_height) / img.getHeight().value()
                                        : SkScalar{0};

                  SkMatrix elem_to_img =
                    SkMatrix::Translate(-img.getX().value(), -img.getY().value());
                  elem_to_img.preScale(sx, sy);

                  out_img.root_to_image = elem_to_img * elem_from_root;
                }

                // Pre-read pixels into CPU memory once.
                SkImageInfo info = SkImageInfo::MakeN32Premul(static_cast<int>(out_img.img_width),
                                                              static_cast<int>(out_img.img_height));

                out_img.row_bytes = out_img.img_width * sizeof(u32);
                out_img.pixels.resize(out_img.img_width * out_img.img_height);

                SkPixmap pix(info, out_img.pixels.data(), out_img.row_bytes);
                if (!skimg->readPixels(nullptr, pix, 0, 0)) {
                  out_img.pixels.clear();
                  out_img.row_bytes = 0;
                }
              }
            }
          }
        }

        out_nodes.push_back(std::move(out_img));
        break;
      }

      case SkSVGTag::kG: {
        const auto& g = static_cast<const SkSVGG&>(node);
        const SkMatrix child_trans = g.getTransform() * parent_transform;
        const SkScalar group_opacity = parent_opacity * resolve_node_opacity(g);

        if (const auto& clip_path = g.getClipPath(); clip_path.isValue()) {
          // Group with clip-path applied.
          const auto& iri = clip_path->iri().iri();
          std::string_view iri_sv{iri.data(), iri.size()};
          const ClipPathGeom& clip_geom = defs.clip_paths.at(iri_sv);

          if (clip_geom.bounds.isEmpty() || !clip_geom.bounds.intersects(viewbox)) {
            return;
          }

          clips.emplace_back(clip_geom.path, clip_geom.bounds);
          collect_group_children(g, child_trans, group_opacity);
          clips.pop_back();
        } else {
          // Normal group, just recurse into children.
          collect_group_children(g, child_trans, group_opacity);
        }
        break;
      }

      case SkSVGTag::kSvg: {
        const auto& container = static_cast<const SkSVGContainer&>(node);
        const SkMatrix child_trans = container.getTransform() * parent_transform;
        const SkScalar svg_opacity = parent_opacity * resolve_node_opacity(container);
        collect_group_children(container, child_trans, svg_opacity);
        break;
      }

      case SkSVGTag::kUse: {
        const auto& use = static_cast<const SkSVGUse&>(node);
        const SkMatrix use_trans = use.getTransform() * parent_transform;
        const SkScalar use_opacity = parent_opacity * resolve_node_opacity(use);

        const auto href = use.getHref().iri();
        std::string_view href_sv{href.data(), href.size()};

        const SkSVGNode* ref_node = defs.id_map.at(href_sv);
        collect_paths_in_paint_order(*ref_node, use_trans, use_opacity);
        break;
      }

      default: break;
    }
  }
};

// -----------------------------------------------------------------------------
// Sampling / compositing
// -----------------------------------------------------------------------------

// Evaluate color for a single sample (x,y) in root SVG coordinates from a
// back-to-front node list.
inline Rgba compute_sample_color_for_list(const std::vector<NodeInfo>& nodes,
                                          const std::vector<Tile>& tiles, const TileParams& tp,
                                          SkScalar x, SkScalar y) {
  if (nodes.empty()) {
    return {.r = 0, .g = 0, .b = 0, .a = 0};
  }

  Rgba out{.r = 0, .g = 0, .b = 0, .a = 0};

  const uz tx = uz(std::clamp(iz(std::floor((x - tp.xoff) * tp.xfactor)), 0Z, iz(tiles_x - 1)));
  const uz ty = uz(std::clamp(iz(std::floor((y - tp.yoff) * tp.yfactor)), 0Z, iz(tiles_y - 1)));

  const Tile& tile = tiles[ty * tiles_x + tx];

  for (std::size_t i : tile.node_indices | std::views::reverse) {
    // Filled path
    auto op_filled = [&](const Path& p) {
      if (!p.bounds.contains(x, y)) {
        return;
      }
      if (!p.path.contains(x, y)) {
        return;
      }
      for (const ClipMask& clip : p.clips) {
        if (!clip.bounds.contains(x, y)) {
          return;
        }
        if (!clip.path.contains(x, y)) {
          return;
        }
      }

      const SkScalar inv_front_a = SkScalar{1} - out.a;
      out.r += p.premul_r * inv_front_a;
      out.g += p.premul_g * inv_front_a;
      out.b += p.premul_b * inv_front_a;
      out.a += p.fill_opacity * inv_front_a;
    };
    // <image>
    auto op_image = [&](const Image& img) {
      if (!img.bounds.contains(x, y)) {
        return;
      }
      for (const ClipMask& clip : img.clips) {
        if (!clip.bounds.contains(x, y)) {
          return;
        }
        if (!clip.path.contains(x, y)) {
          return;
        }
      }

      // Fallback rect if decode failed or no pixels.
      if (!img.decoded || img.pixels.empty()) {
        const SkScalar a = img.opacity;
        constexpr SkScalar r = 1, g = 0, b = 1;
        const SkScalar inv_front_a = SkScalar{1} - out.a;
        out.r += r * a * inv_front_a;
        out.g += g * a * inv_front_a;
        out.b += b * a * inv_front_a;
        out.a += a * inv_front_a;
        return;
      }

      SkPoint p = img.root_to_image.mapPoint(SkPoint{x, y});
      const SkScalar u = p.x();
      const SkScalar v = p.y();

      // Nearest neighbour, clamped to [0, w-1]x[0, h-1].
      auto clamp_to_size = [](SkScalar coord, std::size_t max) -> std::size_t {
        auto f = coord + SkScalar(0.5);
        if (f < 0) {
          f = 0;
        }
        auto i = static_cast<std::size_t>(f);
        if (i >= max) {
          i = max - 1;
        }
        return i;
      };

      const std::size_t ix = clamp_to_size(u, img.img_width);
      const std::size_t iy = clamp_to_size(v, img.img_height);

      const u32* row_ptr = reinterpret_cast<const u32*>(
        reinterpret_cast<const std::byte*>(img.pixels.data()) + iy * img.row_bytes);
      const u32 pixel = row_ptr[ix];

      SkScalar r = SkColorGetR(pixel) * inv255;
      SkScalar g = SkColorGetG(pixel) * inv255;
      SkScalar b = SkColorGetB(pixel) * inv255;
      SkScalar a = SkColorGetA(pixel) * inv255;

      // Modulate by image opacity.
      a *= img.opacity;
      if (a <= 0) {
        return;
      }

      // r,g,b are premul by original a; scale by img.opacity as well.
      r *= img.opacity;
      g *= img.opacity;
      b *= img.opacity;

      const SkScalar inv_front_a = SkScalar{1} - out.a;
      out.r += r * inv_front_a;
      out.g += g * inv_front_a;
      out.b += b * inv_front_a;
      out.a += a * inv_front_a;
    };
    std::visit(Overloaded{op_filled, op_image}, nodes[i]);

    if (out.a >= SkScalar(0.999)) {
      out.a = 1;
      break;
    }
  }

  if (out.a <= 0) {
    return {.r = 0, .g = 0, .b = 0, .a = 0};
  }

  const SkScalar inv_a = SkScalar{1} / out.a;
  return {.r = out.r * inv_a, .g = out.g * inv_a, .b = out.b * inv_a, .a = out.a};
}

// Convenience wrapper.
inline Rgba compute_sample_color(const std::vector<NodeInfo>& nodes, const std::vector<Tile>& tiles,
                                 const TileParams& tp, SkScalar x, SkScalar y) {
  return compute_sample_color_for_list(nodes, tiles, tp, x, y);
}

std::vector<Tile> build_grid(const std::vector<NodeInfo>& nodes, const TileParams& tp) {
  std::vector<Tile> tiles(tiles_x * tiles_y);

  auto add_node_to_tiles = [&](std::size_t idx, const SkRect& b) {
    if (b.isEmpty()) {
      return;
    }

    // convert root coords -> pixel space
    const uz x0 = uz(std::max(0Z, iz(std::floor((b.left() - tp.xoff) * tp.xfactor))));
    const uz x1 = uz(std::min(iz(tiles_x - 1), iz(std::ceil((b.right() - tp.xoff) * tp.xfactor))));
    const uz y0 = uz(std::max(0Z, iz(std::floor((b.top() - tp.yoff) * tp.yfactor))));
    const uz y1 = uz(std::min(iz(tiles_y - 1), iz(std::ceil((b.bottom() - tp.yoff) * tp.yfactor))));

    for (std::size_t ty = y0; ty <= y1; ++ty) {
      for (std::size_t tx = x0; tx <= x1; ++tx) {
        tiles[ty * tiles_x + tx].node_indices.push_back(idx);
      }
    }
  };

  for (std::size_t i = 0; i < nodes.size(); ++i) {
    std::visit(Overloaded{
                 [&](const Path& p) { add_node_to_tiles(i, p.bounds); },
                 [&](const Image& im) { add_node_to_tiles(i, im.bounds); },
               },
               nodes[i]);
  }

  return tiles;
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main(int argc, char** argv) {
  if (argc < 5) {
    fmt::print(stderr, "Usage: {} input.svg width height output.png [samples_x samples_y]\n",
               argv[0]);
    return EXIT_FAILURE;
  }

  const std::filesystem::path svg_file{argv[1]};
  const std::size_t width = static_cast<std::size_t>(std::stoull(argv[2]));
  const std::size_t height = static_cast<std::size_t>(std::stoull(argv[3]));
  const std::filesystem::path out_file{argv[4]};

  std::size_t samples_x = 1;
  std::size_t samples_y = 1;
  if (argc >= 7) {
    samples_x = static_cast<std::size_t>(std::stoull(argv[5]));
    samples_y = static_cast<std::size_t>(std::stoull(argv[6]));
  }

  if (width == 0 || height == 0) {
    fmt::print(stderr, "Width and height must be positive.\n");
    return EXIT_FAILURE;
  }
  if (samples_x == 0 || samples_y == 0) {
    fmt::print(stderr, "samples_x and samples_y must be positive.\n");
    return EXIT_FAILURE;
  }

  // Load SVG and obtain viewBox mapping.
  SkFILEStream stream{svg_file.c_str()};
  if (!stream.isValid()) {
    fmt::print(stderr, "Failed to open SVG file: {}\n", svg_file);
    return EXIT_FAILURE;
  }

  auto data = SkData::MakeFromStream(&stream, stream.getLength());
  if (!data) {
    fmt::print(stderr, "Failed to read SVG file: {}\n", svg_file);
    return EXIT_FAILURE;
  }

  SkMemoryStream mem_stream(data);
  sk_sp<sk::SVGDOM> dom = sk::SVGDOM::MakeFromStream(mem_stream);
  if (!dom) {
    fmt::print(stderr, "Failed to parse SVG file: {}\n", svg_file);
    return EXIT_FAILURE;
  }

  const SkRect viewbox = dom->getRoot()->getViewBox().value();
  const SkScalar svg_x = viewbox.left();
  const SkScalar svg_y = viewbox.top();
  const SkScalar svg_w = viewbox.width();
  const SkScalar svg_h = viewbox.height();
  const SkScalar xscale = svg_w / SkScalar(width);
  const SkScalar yscale = svg_h / SkScalar(height);

  // First pass: defs / clip paths / ids.
  Defs defs{};
  {
    SkMatrix trans{};
    collect_defs(*dom->getRoot(), trans, defs);
  }

  // Second pass: flatten into paint-ordered node list.
  std::vector<NodeInfo> nodes{};
  {
    SkMatrix trans{};
    std::vector<ClipMask> clips{};
    Collector{.out_nodes = nodes, .clips = clips, .defs = defs, .viewbox = viewbox}
      .collect_paths_in_paint_order(*dom->getRoot(), trans, 1);
  }

  TileParams tile_params{
    .xoff = viewbox.left(),
    .yoff = viewbox.top(),
    .xfactor = SkScalar(tiles_x) / viewbox.width(),
    .yfactor = SkScalar(tiles_y) / viewbox.height(),
  };
  const auto grid = build_grid(nodes, tile_params);
  // for (std::size_t y = 0; y < tiles_y; ++y) {
  //   for (std::size_t x = 0; x < tiles_x; ++x) {
  //     fmt::print("{}", grid[y * tiles_x + x].node_indices.empty() ? "." : "x");
  //   }
  //   fmt::print("\n");
  // }

  if (nodes.empty()) {
    fmt::print(stderr, "No drawable nodes found in SVG.\n");
    return EXIT_FAILURE;
  }

  const std::size_t samples_per_pixel = samples_x * samples_y;
  const SkScalar factor_x = xscale / SkScalar(samples_x);
  const SkScalar factor_y = yscale / SkScalar(samples_y);
  const SkScalar inv_samples_per_pixel = SkScalar{1} / SkScalar(samples_per_pixel);
  std::vector<Rgba> sample_colors(samples_per_pixel);

  auto pixels = std::make_unique<u32[]>(height * width);

  fmt::print("Render image ({}×{}, supersampling: {}×{})\n", width, height, samples_x, samples_y);

#pragma omp parallel for firstprivate(sample_colors)
  for (std::size_t iy = 0; iy < height; ++iy) {
    const SkScalar fiy = SkScalar(iy) * yscale;
    u32* row = pixels.get() + iy * width;

    for (std::size_t ix = 0; ix < width; ++ix) {
      const SkScalar fix = SkScalar(ix) * xscale;
      std::size_t s = 0;
      for (std::size_t sy = 0; sy < samples_y; ++sy) {
        const SkScalar fy = fiy + (SkScalar(sy) + SkScalar(0.5)) * factor_y + svg_y;
        for (std::size_t sx = 0; sx < samples_x; ++sx) {
          const SkScalar fx = fix + (SkScalar(sx) + SkScalar(0.5)) * factor_x + svg_x;

          sample_colors[s++] = compute_sample_color(nodes, grid, tile_params, fx, fy);
        }
      }

      SkScalar sumA = 0, sumR = 0, sumG = 0, sumB = 0;
      std::size_t contributing = 0;

      for (std::size_t i = 0; i < samples_per_pixel; ++i) {
        const Rgba c = sample_colors[i];
        const SkScalar a = c.a;
        contributing += (a > 0);
        sumA += a;
        sumR += c.r;
        sumG += c.g;
        sumB += c.b;
      }

      const SkScalar wf = SkScalar{1} / SkScalar(std::max(contributing, 1UZ));

      const auto to_u8 = [](SkScalar v) -> u8 {
        return u8(std::clamp(v * SkScalar{255} + SkScalar(0.5), SkScalar{0}, SkScalar{255}));
      };

      const u8 r = to_u8(sumR * wf);
      const u8 g = to_u8(sumG * wf);
      const u8 b = to_u8(sumB * wf);
      const u8 a = to_u8(sumA * inv_samples_per_pixel);

      row[ix] = (u32(a) << 24) | (u32(r) << 0) | (u32(g) << 8) | (u32(b) << 16);
    }
  }

  SkPixmap pixmap{
    SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kUnpremul_SkAlphaType),
    pixels.get(),
    width * 4,
  };

  SkFILEWStream outStream(out_file.c_str());
  if (!outStream.isValid()) {
    fmt::print(stderr, "Failed to open output file: {}\n", out_file);
    return EXIT_FAILURE;
  }

  SkPngEncoder::Encode(&outStream, pixmap, {});
  fmt::print("Wrote image to {}\n", out_file);

  return EXIT_SUCCESS;
}
