#pragma once
// Draw functions for the single main screen (Spanish UI). Pure: rendering
// from a DrawState snapshot into a Canvas, emitting hit zones — no side
// effects, no audio/model access. The App owns all state.

#include <string>
#include <vector>

#include "canvas.hh"
#include "gui/recorder_controller.h"
#include "gui/theme.h"

namespace gui {

enum Action {
    ActNone = 0,
    ActRecord,
    ActLangField,       // open the language popup
    ActMicField,        // open the capture-device popup
    ActSave,
    ActCopy,            // copy the plain transcript to the clipboard
    ActRetryQuality,    // re-transcribe the same take with the quality preset
    ActRetryLang,       // re-transcribe with the currently selected language
    ActPathField,       // focus the save-path text field
    ActFormatBase = 100,   // + index into kFormats (txt/srt/vtt/json)
    ActPopupBase  = 1000,  // + item index inside the open popup
    ActPopupClose = 9999,  // click outside the popup
};

constexpr const char * kFormats[] = {"txt", "srt", "vtt", "json"};
constexpr int kFormatCount = 4;

struct Hit {
    Rect rect;
    int  action;
};

struct DrawState {
    const RecorderController * ctl = nullptr;

    std::string lang_label;      // "Auto-detectar idioma" | "Spanish (es)"...
    std::string mic_label;       // selected device name or "(ninguno)"
    int         format_sel = 0;  // index into kFormats
    std::string save_path;
    bool        path_focused = false;
    std::string toast;           // transient confirmation line ("Guardado: …")

    enum class Popup { None, Lang, Mic } popup = Popup::None;
    const std::vector<std::string> * popup_items = nullptr;
    int   popup_selected = -1;
    float popup_scroll = 0.0f;

    PointerState ptr;
};

// Draws everything and rebuilds `hits` (cleared first). Popup hit zones are
// emitted last so they win overlap tests when iterated front-to-back.
void draw_main(Canvas & c, const DrawState & st, std::vector<Hit> & hits);

// Popup list row height used by draw_main (for scroll clamping in the App).
float popup_row_height(float screen_h);

} // namespace gui
