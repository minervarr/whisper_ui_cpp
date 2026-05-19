#pragma once

#include <windows.h>

#include <string>

#include "inference/transcribe.h"

namespace ui {

enum class ResultAction {
    Close,
    RetryQuality,        // re-transcribir con preset de calidad (beam search)
    RetryWithLanguage,   // re-transcribir con nuevo idioma (en `new_language`)
};

struct ResultOutcome {
    ResultAction action       = ResultAction::Close;
    std::string  new_language; // válido si action == RetryWithLanguage
};

// Muestra el diálogo modal con la transcripción.
// El Result se referencia internamente; el llamante mantiene la propiedad.
ResultOutcome show_result_dialog(HWND parent, const inference::Result & result);

} // namespace ui
