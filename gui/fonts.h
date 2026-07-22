#pragma once
// Shared font setup for the GUI: load the curve font + cached MSDF atlas once,
// upload the atlas per-Renderer, and build a Canvas wired to both. Used by the
// live app (app.cpp) and the headless UI-capture tool, so the "which fonts,
// wired how" decision lives in exactly one place.

#include <string>
#include <vector>

#include "canvas.hh"

class AssetReader;
class Renderer;

namespace gui {

// Load the curve font and bake/load the MSDF atlas from `cache` (no GPU work).
void init_fonts(AssetReader & assets, const std::string & cache);

// Upload the MSDF atlas into a Renderer. Call once per Renderer after
// init_fonts(); no-op if MSDF is unavailable.
void upload_msdf(Renderer & r, const std::string & cache);

// Default MSDF cache path (beside the app's config dir).
std::string msdf_cache_path();

// Construct a Canvas over `curves`/`msdf_quads` (both cleared) at w×h, wired to
// whichever of the curve font / MSDF atlas loaded. The caller then draws and
// passes the buffers to Renderer::draw(curves, 0, {}, {}, msdf_quads).
Canvas make_canvas(std::vector<float> & curves, std::vector<float> & msdf_quads,
                   uint32_t w, uint32_t h);

} // namespace gui
