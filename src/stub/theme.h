#pragma once

#include <cstdint>

#include "lwi/config.h"

namespace lwi::stub
{

/// Colours are 0xAARRGGBB.
///
/// Defaults are the Locke Werks palette, taken from the inline stylesheet at
/// lockewerks-web/public/main/index.html. That palette is dark only and defines
/// no error or success colour, so those two come from the DeadLetter installer's
/// Catppuccin set, which is the one already shipping on a Locke Werks installer.
///
/// Every value is overridable per product under [ui.theme].
struct Theme
{
    uint32_t background = 0xFF06060E;
    uint32_t surface = 0xFF0C0C1E;
    uint32_t border = 0xFF1A1A2E;
    uint32_t border_hover = 0xFF1E3A5F;
    uint32_t accent = 0xFF00CCCC;
    uint32_t accent_soft = 0x1400E6E6; // rgba(0,230,230,0.08)
    uint32_t text = 0xFFE8E8EE;
    uint32_t text_body = 0xFFC0C0C8;
    uint32_t text_muted = 0xFF888888;
    uint32_t text_faint = 0xFF666666;
    uint32_t error = 0xFFF38BA8;
    uint32_t success = 0xFFA6E3A1;
    uint32_t warning = 0xFFF9E2AF;

    // Radii and metrics in DIPs, scaled at draw time by the window's DPI.
    float radius_card = 16.0f;
    float radius_button = 8.0f;

    static Theme from_config(const Config& config);
};

/// Design size in DIPs. Laid out at 96 dpi and scaled, rather than recomputed
/// per DPI, so a 150% display shows the same composition rather than a
/// differently proportioned one.
inline constexpr float kWindowWidth = 640.0f;
inline constexpr float kWindowHeight = 520.0f;

} // namespace lwi::stub
