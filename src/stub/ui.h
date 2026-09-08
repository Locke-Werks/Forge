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
#include "ops.h"
#include "theme.h"

namespace lwi::stub
{

enum class Page
{
    License,
    Options,
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
    /// Ends the install successfully, listing anything that did not go to plan.
    ///
    /// The silent path has always printed these; the wizard threw them away,
    /// which meant a hook that timed out was reported to nobody and hidden from
    /// the one person watching. A completed install that quietly did less than
    /// it said it would is the failure this closes.
    void finish_ok(std::vector<std::wstring> warnings = {});
    void finish_error(std::wstring message);

    [[nodiscard]] bool cancelled() const { return cancelled_.load(); }
    [[nodiscard]] const std::wstring& install_dir() const { return install_dir_; }
    void set_install_dir(std::wstring dir) { install_dir_ = std::move(dir); }

    /// The options as the config declared them and the command line amended
    /// them. The wizard owns them from here until the install starts, so the
    /// page and the engine cannot end up with two different answers.
    ///
    /// Invalidates, because init() shows the window and paints it once before
    /// this is ever called. Without that the license page's button read INSTALL
    /// on an installer that had an options page still to come, and corrected
    /// itself the first time the mouse moved.
    void set_options(std::vector<InstallOption> options)
    {
        options_ = std::move(options);
        if (hwnd_ != nullptr)
        {
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    [[nodiscard]] const std::vector<InstallOption>& options() const { return options_; }

  private:
    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(HWND, UINT, WPARAM, LPARAM);

    bool create_device_resources();
    void discard_device_resources();
    void paint();
    void layout_hit_regions(float width, float height);
    void apply_window_attributes();
    void on_click(float x, float y);
    void begin_install();

    /// Leaves the license page: to the options when there are any to show,
    /// straight into the install when there are not, so an installer that
    /// declares no options looks exactly as it did before.
    void advance_from_license();

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

    std::vector<InstallOption> options_;
    std::vector<D2D1_RECT_F> rc_options_;
    // Which row the keyboard is on. -1 until an arrow key is pressed, so a
    // mouse user never sees a focus ring they did not ask for.
    int option_focus_ = -1;

    std::mutex state_lock_;
    float progress_ = 0.0f;
    std::wstring status_;
    std::wstring error_;
    std::vector<std::wstring> warnings_;

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
