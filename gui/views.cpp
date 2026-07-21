#include "gui/views.h"

#include "text_util.hh"
#include "widgets.hh"
#include "msdf.hh"

namespace gui {

namespace {

// The popup covers the central band of the window.
Rect popup_area(float w, float h)
{
    return { w * 0.15f, h * 0.12f, w * 0.70f, h * 0.72f };
}

} // namespace

float popup_row_height(float screen_h)
{
    return screen_h * 0.055f;
}

void draw_main(Canvas & c, const DrawState & st, std::vector<Hit> & hits)
{
    hits.clear();

    const float W = c.w();
    const float H = c.h();
    const float pad = c.pad();
    TypeScale ts(H);

    c.clear(pal::bg);

    const RecorderController & ctl = *st.ctl;
    float y = pad;

    // ── status row: LED dot · status text · timer ──────────────────────────
    const float led_d = ts.body * 1.1f;
    c.rect(pad, y, led_d, led_d, led_to_color(ctl.led_color()), led_d * 0.5f);
    c.text(ctl.status_line(), pad + led_d + pad * 0.5f, y, ts.body, pal::text);
    c.textRight(ctl.timer_text(), W - pad, y, ts.body,
                ctl.state() == UiState::Recording ? pal::red : pal::dim);
    y += led_d + pad;

    // ── record button ──────────────────────────────────────────────────────
    {
        const float bw = W * 0.42f;
        const float bh = H * 0.11f;
        Rect r{ (W - bw) / 2.0f, y, bw, bh };
        const char * label = "Grabar";
        Color bg = pal::btnIdle;
        switch (ctl.state()) {
            case UiState::Recording:    label = "Detener";        bg = pal::btnRec;  break;
            case UiState::Transcribing: label = "Transcribiendo…"; bg = pal::track;   break;
            case UiState::Loading:      label = "Cargando…";      bg = pal::track;   break;
            case UiState::Error:        label = "Error";          bg = pal::track;   break;
            default: break;
        }
        bool hover = r.contains(st.ptr.x, st.ptr.y);
        if (hover && ctl.state() == UiState::Ready) bg = pal::accent;
        c.button(r.x, r.y, r.w, r.h, label, bg, pal::text, bh * 0.25f);
        hits.push_back({r, ActRecord});
        y += bh + pad;
    }

    // ── language + microphone rows ─────────────────────────────────────────
    {
        const float rh = H * 0.062f;
        const bool no_popup = st.popup == DrawState::Popup::None;
        Rect lang_row{ pad, y, W - 2 * pad, rh };
        widgets::drawDropdownField(c, lang_row, "Idioma", st.lang_label,
                                   no_popup && lang_row.contains(st.ptr.x, st.ptr.y));
        hits.push_back({lang_row, ActLangField});
        y += rh + pad * 0.5f;

        Rect mic_row{ pad, y, W - 2 * pad, rh };
        widgets::drawDropdownField(c, mic_row, "Micrófono", st.mic_label,
                                   no_popup && mic_row.contains(st.ptr.x, st.ptr.y));
        hits.push_back({mic_row, ActMicField});
        y += rh + pad;
    }

    // ── transcript area ────────────────────────────────────────────────────
    {
        const float bottom_h = H * 0.19f;                 // reserved for actions
        const float th = H - y - bottom_h - pad;
        Rect area{ pad, y, W - 2 * pad, th };
        c.rect(area.x, area.y, area.w, area.h, pal::panel, pad * 0.3f);

        std::string body = ctl.transcript_preview();
        if (body.empty()) {
            const char * hint =
                (ctl.state() == UiState::Recording)    ? "Habla ahora…" :
                (ctl.state() == UiState::Transcribing) ? "Procesando el audio…"
                                                       : "Aquí aparecerá la transcripción.";
            c.text(hint, area.x + pad * 0.6f, area.y + pad * 0.6f, ts.small, pal::dim);
        } else {
            std::vector<std::string> lines;
            wrapText(c, body, area.w - pad * 1.2f, ts.small, FontStyle::Roman, lines);
            float ly = area.y + pad * 0.6f;
            const float lh = ts.small * 1.45f;
            // Show the tail when the take is longer than the panel.
            size_t max_lines = (size_t) ((area.h - pad * 1.2f) / lh);
            size_t first = lines.size() > max_lines ? lines.size() - max_lines : 0;
            for (size_t i = first; i < lines.size(); ++i) {
                c.text(lines[i], area.x + pad * 0.6f, ly, ts.small, pal::text);
                ly += lh;
            }
        }
        y = area.y + area.h + pad * 0.5f;
    }

    // ── result actions: format segmented · path field · save · retries ─────
    {
        const bool has_result = ctl.last_result() != nullptr &&
                                ctl.last_result()->error.empty() &&
                                !ctl.last_result()->segments.empty();
        const float rh = H * 0.055f;

        // format selector + save path + Guardar
        Rect seg{ pad, y, W * 0.30f, rh };
        widgets::drawSegmented(c, seg,
                               {kFormats[0], kFormats[1], kFormats[2], kFormats[3]},
                               st.format_sel);
        for (int i = 0; i < kFormatCount; ++i)
            hits.push_back({widgets::segmentRectAt(seg, kFormatCount, i),
                            ActFormatBase + i});

        Rect path{ seg.x + seg.w + pad * 0.5f, y,
                   W - seg.w - 3.5f * pad - W * 0.14f, rh };
        c.rect(path.x, path.y, path.w, path.h,
               st.path_focused ? pal::panel2 : pal::panel, rh * 0.2f);
        std::string shown = truncateToWidth(c, st.save_path, path.w - pad,
                                            ts.small, FontStyle::Roman);
        c.text(shown + (st.path_focused ? "_" : ""),
               path.x + pad * 0.5f, path.y + (rh - ts.small) / 2,
               ts.small, pal::text);
        hits.push_back({path, ActPathField});

        Rect save{ path.x + path.w + pad * 0.5f, y, W * 0.14f, rh };
        c.button(save.x, save.y, save.w, save.h, "Guardar",
                 has_result ? pal::accent : pal::track, pal::text, rh * 0.25f);
        if (has_result) hits.push_back({save, ActSave});
        y += rh + pad * 0.5f;

        // retry row + toast
        Rect rq{ pad, y, W * 0.34f, rh };
        c.button(rq.x, rq.y, rq.w, rq.h, "Reintentar con más calidad",
                 has_result ? pal::btnIdle : pal::track, pal::text, rh * 0.25f);
        if (has_result) hits.push_back({rq, ActRetryQuality});

        Rect rl{ rq.x + rq.w + pad * 0.5f, y, W * 0.34f, rh };
        c.button(rl.x, rl.y, rl.w, rl.h, "Reintentar con este idioma",
                 has_result ? pal::btnIdle : pal::track, pal::text, rh * 0.25f);
        if (has_result) hits.push_back({rl, ActRetryLang});

        if (!st.toast.empty()) {
            c.textRight(st.toast, W - pad, y + (rh - ts.small) / 2,
                        ts.small, pal::green);
        }
    }

    // ── popup (language / device picker) — drawn last, wins hit-tests ──────
    if (st.popup != DrawState::Popup::None && st.popup_items) {
        // Modal: only the popup's zones are clickable while it is open. Rows
        // go before the full-screen close backdrop — the App takes the first
        // hit in push order.
        hits.clear();
        Rect area = popup_area(W, H);

        // Clean layer: cull ALL base-screen text (MSDF composites after
        // geometry, so draw order alone can't hide it), then dim the whole
        // base UI with a scrim before the panel goes on top.
        c.occlude(0, 0, W, H);
        c.rect(0, 0, W, H, with_alpha(pal::bg, 0.60f));

        // Pointer -> hovered item index, so the row under the cursor
        // highlights live (no click needed).
        int hover = -1;
        const float row_h = popup_row_height(H);
        if (area.contains(st.ptr.x, st.ptr.y)) {
            int i = (int) ((st.ptr.y - area.y + st.popup_scroll) / row_h);
            if (i >= 0 && i < (int) st.popup_items->size()) hover = i;
        }

        auto rows = widgets::drawScrollList(c, area, *st.popup_items,
                                            st.popup_selected, st.popup_scroll,
                                            row_h, hover);
        for (const auto & row : rows)
            hits.push_back({row.rect, ActPopupBase + row.index});
        hits.push_back({{0, 0, W, H}, ActPopupClose});
    }
}

} // namespace gui
