#include "gui/fonts.h"

#include "core/settings.h"

#include "font.hh"
#include "msdf.hh"
#include "renderer.hh"

#include <cstdint>
#include <cstdio>
#include <filesystem>

namespace gui {

namespace {

// Curve font (filled glyphs) + cached MSDF atlas, the scanersito pipeline.
Font     g_font;
bool     g_font_ok = false;
MsdfFont g_msdf;
bool     g_msdf_ok = false;

constexpr const char * kFontRegular = "fonts/NewCM10-Book.otf";

} // namespace

void init_fonts(AssetReader & assets, const std::string & cache)
{
    std::vector<uint8_t> bytes;
    if (assets.read(kFontRegular, bytes))
        g_font_ok = g_font.loadFromMemory(bytes.data(), bytes.size());

    if (g_msdf.generate(assets, kFontRegular, cache.c_str()))
        g_msdf_ok = g_msdf.valid();
    if (!g_msdf_ok)
        std::fprintf(stderr, "[!] MSDF unavailable — curve text only\n");
}

void upload_msdf(Renderer & r, const std::string & cache)
{
    if (!g_msdf_ok) return;
    g_msdf.ensureAtlasLoaded(cache.c_str());
    r.initMsdf(g_msdf);
    g_msdf.releaseAtlasPixels();
}

std::string msdf_cache_path()
{
    std::string cfg = cfg::config_path();          // ensures the dir exists
    if (cfg.empty()) return "msdf.cache";
    return std::filesystem::path(cfg).parent_path().string() + "/msdf.cache";
}

Canvas make_canvas(std::vector<float> & curves, std::vector<float> & msdf_quads,
                   uint32_t w, uint32_t h)
{
    curves.clear();
    msdf_quads.clear();
    Canvas c(curves, w, h, g_font_ok ? &g_font : nullptr, 0, 0, 0, 0);
    if (g_msdf_ok) c.useMsdf(&g_msdf, &msdf_quads);
    return c;
}

} // namespace gui
