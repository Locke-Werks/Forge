#include "theme.h"

namespace lwi::stub
{

Theme Theme::from_config(const Config& config)
{
    Theme t;
    t.background = config.get_color("ui.theme.background", t.background);
    t.surface = config.get_color("ui.theme.surface", t.surface);
    t.border = config.get_color("ui.theme.border", t.border);
    t.border_hover = config.get_color("ui.theme.border_hover", t.border_hover);
    t.accent = config.get_color("ui.theme.accent", t.accent);
    t.accent_soft = config.get_color("ui.theme.accent_soft", t.accent_soft);
    t.text = config.get_color("ui.theme.text", t.text);
    t.text_body = config.get_color("ui.theme.text_body", t.text_body);
    t.text_muted = config.get_color("ui.theme.text_muted", t.text_muted);
    t.text_faint = config.get_color("ui.theme.text_faint", t.text_faint);
    t.error = config.get_color("ui.theme.error", t.error);
    t.success = config.get_color("ui.theme.success", t.success);
    return t;
}

} // namespace lwi::stub
