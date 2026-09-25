// The editor's colors, shared by the editor and its start screen.
#pragma once

#include <thistle.hpp>

namespace editor::theme {
using thistle::rgb;
inline const thistle::rgba chrome = rgb(0.10f, 0.10f, 0.11f);
inline const thistle::rgba panel = rgb(0.16f, 0.16f, 0.17f);
inline const thistle::rgba field = rgb(0.22f, 0.22f, 0.24f);
inline const thistle::rgba field_hover = rgb(0.27f, 0.27f, 0.30f);
inline const thistle::rgba field_active = rgb(0.18f, 0.30f, 0.40f);
inline const thistle::rgba text = rgb(0.88f, 0.88f, 0.88f);
inline const thistle::rgba dim = rgb(0.58f, 0.58f, 0.62f);
inline const thistle::rgba faint = rgb(0.42f, 0.42f, 0.45f);
inline const thistle::rgba accent = rgb(0.95f, 0.55f, 0.18f); // Blender-ish orange: the selection
inline const thistle::rgba bad = rgb(0.95f, 0.38f, 0.35f);
inline const thistle::rgba good = rgb(0.45f, 0.82f, 0.45f);
inline const thistle::rgba axis_x = rgb(0.92f, 0.28f, 0.30f);
inline const thistle::rgba axis_y = rgb(0.45f, 0.82f, 0.25f);
inline const thistle::rgba axis_z = rgb(0.25f, 0.52f, 0.95f);
inline const thistle::rgba hover = rgb(1.0f, 0.88f, 0.25f);
} // namespace editor::theme
