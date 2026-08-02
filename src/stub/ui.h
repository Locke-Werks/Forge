#pragma once

#include <windows.h>

#include <d2d1.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "lwi/config.h"
#include "lwi/container.h"
#include "theme.h"

namespace lwi::stub
{

enum class Page
{
    License,
    Progress,
    Complete,
    Error,
};

/// The installer window.
///
/// Deliberately NOT the WS_POPUP | WS_EX_LAYERED frameless window the DeadLetter
/// installer uses. A layered per-pixel-alpha window can never be rounded by DWM,
/// gets no system shadow, never receives WM_PAINT, and cannot render ClearType.
/// Keeping WS_THICKFRAME and WS_CAPTION and zeroing only the painted frame in
/// WM_NCCALCSIZE keeps DWM's policy applicable: system shadow on Windows 10,
/// rounded corners on Windows 11, and clean degradation between them.
class Wizard
{
  public:
    using InstallFn = std::function<void()>;

    bool init(HINSTANCE instance, const Config& config, const Theme& theme, bool dev_build);
    int run();

    void on_install(InstallFn fn) { install_ = std::move(fn); }

    // Called from the worker thread.
    void set_progress(float fraction, std::wstring status);
    void finish_ok();
    void finish_error(std::wstring message);

    [[nodiscard]] bool cancelled() const { return cancelled_.load(); }
    [[nodiscard]] const std::wstring& install_dir() const { return install_dir_; }
    void set_install_dir(std::wstring dir) { install_dir_ = std::move(dir); }

  private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(HWND, UINT, WPARAM, LPARAM);

    bool create_device_resources();
    void discard_device_resources();
    void paint();
    void layout_hit_regions(float width, float height);
    void apply_window_attributes();
    void on_click(float x, float y);

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    float dpi_scale_ = 1.0f;

    Theme theme_;
    bool dev_build_ = false;

    std::wstring product_;
    std::wstring version_;
    std::wstring publisher_;
    std::wstring description_;
    std::wstring warning_;
    std::wstring license_;
    std::wstring install_dir_;

    Page page_ = Page::License;
    bool accepted_ = false;
    bool hover_primary_ = false;
    float scroll_ = 0.0f;
    float scroll_max_ = 0.0f;

    std::mutex state_lock_;
    float progress_ = 0.0f;
    std::wstring status_;
    std::wstring error_;

    std::atomic<bool> cancelled_{false};
    InstallFn install_;

    D2D1_RECT_F rc_checkbox_{};
    D2D1_RECT_F rc_primary_{};
    D2D1_RECT_F rc_close_{};
    D2D1_RECT_F rc_license_{};

    struct Resources;
    Resources* res_ = nullptr;
};

} // namespace lwi::stub
