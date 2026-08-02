#include "ui.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite_1.h>
#include <windowsx.h> // GET_X_LPARAM / GET_Y_LPARAM
#include <wrl/client.h>

#include <algorithm>
#include <thread>

#include "lwi/win_file.h"

using Microsoft::WRL::ComPtr;

namespace lwi::stub
{
namespace
{

constexpr wchar_t kWindowClass[] = L"LwiInstallerWindow";
constexpr UINT_PTR kAnimationTimer = 1;

// USER_TIMER_MINIMUM is 10 ms and anything smaller is silently clamped to it,
// so asking for 8 would quietly become 10 anyway. WM_TIMER is also a low
// priority synthesised message, so this is a repaint cadence, not a frame
// clock; every animation is driven by elapsed time rather than by tick count.
constexpr UINT kAnimationIntervalMs = 16;

// Windows 11 build 22000 for the corner preference, and the dark-mode attribute
// moved from 19 to 20 partway through Windows 10's life. Both are hints: they
// are applied and their results ignored.
constexpr DWORD kDwmWindowCornerPreference = 33;
constexpr DWORD kDwmUseImmersiveDarkMode = 20;
constexpr DWORD kDwmUseImmersiveDarkModeLegacy = 19;
constexpr DWORD kDwmCornerRound = 2;

D2D1_COLOR_F to_color(uint32_t argb)
{
    const float a = static_cast<float>((argb >> 24) & 0xFF) / 255.0f;
    const float r = static_cast<float>((argb >> 16) & 0xFF) / 255.0f;
    const float g = static_cast<float>((argb >> 8) & 0xFF) / 255.0f;
    const float b = static_cast<float>(argb & 0xFF) / 255.0f;
    return D2D1::ColorF(r, g, b, a);
}

bool contains(const D2D1_RECT_F& r, float x, float y)
{
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

} // namespace

struct Wizard::Resources
{
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1HwndRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;

    ComPtr<IDWriteTextFormat> eyebrow;
    ComPtr<IDWriteTextFormat> title;
    ComPtr<IDWriteTextFormat> body;
    ComPtr<IDWriteTextFormat> small_text;
    ComPtr<IDWriteTextFormat> mono;
    ComPtr<IDWriteTextFormat> button;
};

bool Wizard::init(HINSTANCE instance, const Config& config, const Theme& theme, bool dev_build)
{
    instance_ = instance;
    theme_ = theme;
    dev_build_ = dev_build;
    res_ = new Resources();

    product_ = to_wide(config.get("product.name", "Application"));
    version_ = to_wide(config.get("product.version"));
    publisher_ = to_wide(config.get("product.publisher", "Locke Werks"));
    description_ = to_wide(config.get("product.description"));
    warning_ = to_wide(config.get("ui.warning"));
    license_ = to_wide(config.get("ui.license_text"));

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, res_->d2d.GetAddressOf())))
    {
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(res_->dwrite.GetAddressOf()))))
    {
        return false;
    }

    const auto make_format = [&](float size, DWRITE_FONT_WEIGHT weight,
                                 ComPtr<IDWriteTextFormat>& out, const wchar_t* family = L"Segoe UI") {
        return SUCCEEDED(res_->dwrite->CreateTextFormat(family, nullptr, weight,
                                                        DWRITE_FONT_STYLE_NORMAL,
                                                        DWRITE_FONT_STRETCH_NORMAL, size, L"en-us",
                                                        out.GetAddressOf()));
    };

    if (!make_format(11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, res_->eyebrow) ||
        !make_format(26.0f, DWRITE_FONT_WEIGHT_BOLD, res_->title) ||
        !make_format(14.0f, DWRITE_FONT_WEIGHT_NORMAL, res_->body) ||
        !make_format(12.0f, DWRITE_FONT_WEIGHT_NORMAL, res_->small_text) ||
        !make_format(12.0f, DWRITE_FONT_WEIGHT_NORMAL, res_->mono, L"Consolas") ||
        !make_format(12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, res_->button))
    {
        return false;
    }

    // line-height 1.7 on body copy, matching the brand's web stylesheet.
    res_->body->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 14.0f * 1.7f, 14.0f * 1.35f);

    // Button labels are centred in both axes so the label can be drawn into the
    // button rectangle directly instead of against a hand-computed baseline
    // that drifts every time the metrics change.
    res_->button->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    res_->button->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);


    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Wizard::wnd_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);

    // GetDpiForSystem is only the starting point. The window may open on a
    // monitor with a different scale, and WM_DPICHANGED updates this.
    dpi_scale_ = static_cast<float>(GetDpiForSystem()) / 96.0f;

    const int width = static_cast<int>(kWindowWidth * dpi_scale_);
    const int height = static_cast<int>(kWindowHeight * dpi_scale_);
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;

    const std::wstring caption = product_ + L" Setup";

    // WS_THICKFRAME and WS_CAPTION both stay. DWM requires exactly those styles
    // before it will round the corners or draw a shadow, so removing
    // WS_THICKFRAME to prevent resizing puts the window in the "cannot be
    // rounded" bucket and costs the shadow too. Resizing is prevented instead
    // by WM_NCHITTEST, which never returns a resize border, and the frame is
    // hidden by WM_NCCALCSIZE rather than by dropping the style.
    hwnd_ = CreateWindowExW(0, kWindowClass, caption.c_str(),
                            WS_OVERLAPPEDWINDOW & ~(WS_MAXIMIZEBOX | WS_MINIMIZEBOX), x, y, width,
                            height, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr)
    {
        return false;
    }

    // The window may have opened on a monitor whose scale differs from the
    // system one, in which case it was created at the wrong size. Re-measure
    // against the window itself and correct before it is shown, so nothing
    // flashes at the wrong size.
    const float window_scale = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
    if (window_scale != dpi_scale_)
    {
        dpi_scale_ = window_scale;
        SetWindowPos(hwnd_, nullptr, 0, 0, static_cast<int>(kWindowWidth * dpi_scale_),
                     static_cast<int>(kWindowHeight * dpi_scale_),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    apply_window_attributes();
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    return true;
}

void Wizard::apply_window_attributes()
{
    // Both are hints. Failure means an older build that does not support them,
    // which degrades to a square window with the default title bar colour.
    const BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkMode, &dark, sizeof(dark))))
    {
        DwmSetWindowAttribute(hwnd_, kDwmUseImmersiveDarkModeLegacy, &dark, sizeof(dark));
    }

    const DWORD corner = kDwmCornerRound;
    DwmSetWindowAttribute(hwnd_, kDwmWindowCornerPreference, &corner, sizeof(corner));

    // Force a frame recalculation. The non-client area is computed once during
    // CreateWindowEx, and a WM_NCCALCSIZE handler installed by that same call
    // does not retroactively apply to it. Without this the window keeps a
    // standard caption bar and the custom frame silently does nothing.
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

int Wizard::run()
{
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        // IsDialogMessage gives Tab, Shift-Tab and Enter their standard meaning
        // without a dialog template.
        if (!IsDialogMessageW(hwnd_, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK Wizard::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Wizard* self = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<Wizard*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    else
    {
        self = reinterpret_cast<Wizard*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self != nullptr)
    {
        return self->handle(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Wizard::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_NCCALCSIZE:
        if (wp == TRUE)
        {
            // Zero the visual frame while keeping the styles that let DWM round
            // and shadow the window.
            return 0;
        }
        break;

    case WM_NCHITTEST:
    {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        const float y = static_cast<float>(pt.y) / dpi_scale_;
        // The header strip drags the window, except where the close button is.
        if (y < 56.0f)
        {
            const float x = static_cast<float>(pt.x) / dpi_scale_;
            if (!contains(rc_close_, x, y))
            {
                return HTCAPTION;
            }
        }
        return HTCLIENT;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        paint();
        ValidateRect(hwnd, nullptr);
        return 0;

    case WM_DPICHANGED:
    {
        dpi_scale_ = static_cast<float>(HIWORD(wp)) / 96.0f;
        const auto* suggested = reinterpret_cast<const RECT*>(lp);
        // The suggested rectangle must be honoured or the cursor drifts out of
        // the window while it is being dragged between monitors.
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        discard_device_resources();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_SIZE:
        if (res_ != nullptr && res_->target)
        {
            res_->target->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_MOUSEMOVE:
    {
        const float x = static_cast<float>(GET_X_LPARAM(lp)) / dpi_scale_;
        const float y = static_cast<float>(GET_Y_LPARAM(lp)) / dpi_scale_;
        const bool hover = contains(rc_primary_, x, y);
        if (hover != hover_primary_)
        {
            hover_primary_ = hover;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        on_click(static_cast<float>(GET_X_LPARAM(lp)) / dpi_scale_,
                 static_cast<float>(GET_Y_LPARAM(lp)) / dpi_scale_);
        return 0;

    case WM_MOUSEWHEEL:
        if (page_ == Page::License)
        {
            const int delta = GET_WHEEL_DELTA_WPARAM(wp);
            scroll_ = (std::max)(0.0f, (std::min)(scroll_max_, scroll_ - delta * 0.4f));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
        {
            if (page_ == Page::Progress)
            {
                cancelled_.store(true);
            }
            else
            {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
        }
        else if (wp == VK_SPACE && page_ == Page::License)
        {
            accepted_ = !accepted_;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_TIMER:
        if (wp == kAnimationTimer)
        {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_CLOSE:
        if (page_ == Page::Progress)
        {
            cancelled_.store(true);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(page_ == Page::Complete ? 0 : 1602);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Wizard::on_click(float x, float y)
{
    if (contains(rc_close_, x, y))
    {
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
    }

    if (page_ == Page::License)
    {
        if (contains(rc_checkbox_, x, y))
        {
            accepted_ = !accepted_;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (contains(rc_primary_, x, y) && accepted_)
        {
            page_ = Page::Progress;
            SetTimer(hwnd_, kAnimationTimer, kAnimationIntervalMs, nullptr);
            InvalidateRect(hwnd_, nullptr, FALSE);

            if (install_)
            {
                std::thread(install_).detach();
            }
            return;
        }
    }
    else if (page_ == Page::Complete || page_ == Page::Error)
    {
        if (contains(rc_primary_, x, y))
        {
            DestroyWindow(hwnd_);
        }
    }
}

void Wizard::set_progress(float fraction, std::wstring status)
{
    {
        std::lock_guard<std::mutex> lock(state_lock_);
        progress_ = fraction;
        status_ = std::move(status);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Wizard::finish_ok()
{
    {
        std::lock_guard<std::mutex> lock(state_lock_);
        progress_ = 1.0f;
    }
    page_ = Page::Complete;
    KillTimer(hwnd_, kAnimationTimer);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void Wizard::finish_error(std::wstring message)
{
    {
        std::lock_guard<std::mutex> lock(state_lock_);
        error_ = std::move(message);
    }
    page_ = Page::Error;
    KillTimer(hwnd_, kAnimationTimer);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool Wizard::create_device_resources()
{
    if (res_->target)
    {
        return true;
    }

    RECT rc{};
    GetClientRect(hwnd_, &rc);

    const HRESULT hr = res_->d2d->CreateHwndRenderTarget(
        D2D1::RenderTargetProperties(),
        D2D1::HwndRenderTargetProperties(
            hwnd_, D2D1::SizeU(static_cast<UINT32>(rc.right - rc.left),
                               static_cast<UINT32>(rc.bottom - rc.top))),
        res_->target.GetAddressOf());
    if (FAILED(hr))
    {
        return false;
    }

    // Pin the render target to 96 dpi so GetSize() reports PIXELS.
    //
    // By default a render target inherits the system DPI and reports its size
    // in DIPs, which means the scale is applied once by D2D and once again by
    // the transform below: the layout is computed against a width that is not
    // the width being drawn, and the composition overflows the window. Owning
    // the scaling in exactly one place is what makes a fixed 640x520 design
    // land correctly at 100, 125, 150 and 175 percent.
    res_->target->SetDpi(96.0f, 96.0f);

    // Grayscale rather than ClearType. Text inside a PushLayer clip silently
    // downgrades to grayscale unless the layer is initialised for ClearType, so
    // picking grayscale globally is what makes the clipped and unclipped text
    // look the same.
    res_->target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    res_->target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
                                        res_->brush.GetAddressOf());
    return true;
}

void Wizard::discard_device_resources()
{
    if (res_ != nullptr)
    {
        res_->brush.Reset();
        res_->target.Reset();
    }
}

void Wizard::layout_hit_regions(float width, float height)
{
    rc_close_ = D2D1::RectF(width - 44.0f, 12.0f, width - 16.0f, 40.0f);

    // The warning sits between the card and the checkbox and gets its own band.
    // Overlapping it onto the card made a two-line warning unreadable against
    // the license text behind it.
    const float warning_band = warning_.empty() ? 0.0f : 40.0f;
    rc_license_ = D2D1::RectF(32.0f, 168.0f, width - 32.0f, height - 108.0f - warning_band);
    rc_checkbox_ = D2D1::RectF(32.0f, height - 94.0f, 52.0f, height - 74.0f);
    rc_primary_ = D2D1::RectF(width - 192.0f, height - 60.0f, width - 32.0f, height - 20.0f);
}

void Wizard::paint()
{
    if (!create_device_resources())
    {
        return;
    }

    ID2D1HwndRenderTarget* rt = res_->target.Get();
    ID2D1SolidColorBrush* brush = res_->brush.Get();

    // GetClientRect, not ID2D1RenderTarget::GetSize. GetSize reports DIPs
    // against whatever DPI the target carries, so combining it with our own
    // scale transform applies the scale twice and the layout overflows the
    // window. GetClientRect is unambiguously pixels.
    RECT client{};
    GetClientRect(hwnd_, &client);
    const float width = static_cast<float>(client.right - client.left) / dpi_scale_;
    const float height = static_cast<float>(client.bottom - client.top) / dpi_scale_;
    if (width <= 0.0f || height <= 0.0f)
    {
        return;
    }

    layout_hit_regions(width, height);

    rt->BeginDraw();
    rt->SetTransform(D2D1::Matrix3x2F::Scale(dpi_scale_, dpi_scale_));
    rt->Clear(to_color(theme_.background));

    const auto set = [&](uint32_t argb) { brush->SetColor(to_color(argb)); };

    const auto text = [&](std::wstring_view s, IDWriteTextFormat* format, const D2D1_RECT_F& rect,
                          uint32_t color) {
        set(color);
        rt->DrawTextW(s.data(), static_cast<UINT32>(s.size()), format, rect, brush,
                      D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    // Letter-spaced text. The brand tracks its uppercase display type: 0.2em on
    // eyebrows, 0.15em on headings, 0.1em on buttons. Character spacing is only
    // settable on IDWriteTextLayout1, per text range, not on a text format, so
    // tracked strings need a layout per draw rather than a one-time format
    // tweak. Without it the headings read as ordinary bold text, not as the
    // brand's.
    const auto tracked = [&](std::wstring_view s, IDWriteTextFormat* format,
                             const D2D1_RECT_F& rect, uint32_t color, float em, float font_size) {
        if (s.empty())
        {
            return;
        }
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(res_->dwrite->CreateTextLayout(s.data(), static_cast<UINT32>(s.size()), format,
                                                  rect.right - rect.left, rect.bottom - rect.top,
                                                  layout.GetAddressOf())))
        {
            text(s, format, rect, color);
            return;
        }

        ComPtr<IDWriteTextLayout1> layout1;
        if (SUCCEEDED(layout.As(&layout1)))
        {
            const DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(s.size())};
            layout1->SetCharacterSpacing(0.0f, font_size * em, 0.0f, range);
        }

        set(color);
        rt->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), brush,
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    const auto fill_round = [&](const D2D1_RECT_F& rect, float radius, uint32_t color) {
        set(color);
        rt->FillRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush);
    };

    const auto stroke_round = [&](const D2D1_RECT_F& rect, float radius, uint32_t color) {
        set(color);
        rt->DrawRoundedRectangle(D2D1::RoundedRect(rect, radius, radius), brush, 1.0f);
    };

    // Header. The "//" prefix is a recurring mark in the Locke Werks and
    // Specter Point systems and carries into the installer.
    tracked(L"// INSTALL", res_->eyebrow.Get(), D2D1::RectF(32.0f, 60.0f, width - 32.0f, 82.0f),
            theme_.accent, 0.20f, 11.0f);

    std::wstring heading = product_;
    std::transform(heading.begin(), heading.end(), heading.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
    tracked(heading, res_->title.Get(), D2D1::RectF(32.0f, 84.0f, width - 32.0f, 126.0f),
            theme_.text, 0.15f, 26.0f);

    if (!description_.empty())
    {
        text(description_, res_->small_text.Get(),
             D2D1::RectF(32.0f, 126.0f, width - 32.0f, 152.0f), theme_.text_body);
    }

    // Close button.
    set(theme_.text_muted);
    rt->DrawLine(D2D1::Point2F(rc_close_.left + 9.0f, rc_close_.top + 9.0f),
                 D2D1::Point2F(rc_close_.right - 9.0f, rc_close_.bottom - 9.0f), brush, 1.4f);
    rt->DrawLine(D2D1::Point2F(rc_close_.right - 9.0f, rc_close_.top + 9.0f),
                 D2D1::Point2F(rc_close_.left + 9.0f, rc_close_.bottom - 9.0f), brush, 1.4f);

    switch (page_)
    {
    case Page::License:
    {
        fill_round(rc_license_, theme_.radius_card, theme_.surface);
        stroke_round(rc_license_, theme_.radius_card, theme_.border);

        const D2D1_RECT_F inner = D2D1::RectF(rc_license_.left + 20.0f, rc_license_.top + 16.0f,
                                              rc_license_.right - 20.0f, rc_license_.bottom - 16.0f);
        rt->PushAxisAlignedClip(inner, D2D1_ANTIALIAS_MODE_ALIASED);
        const D2D1_RECT_F scrolled =
            D2D1::RectF(inner.left, inner.top - scroll_, inner.right, inner.top - scroll_ + 4000.0f);
        text(license_.empty() ? L"No license text was supplied." : license_, res_->body.Get(),
             scrolled, theme_.text_body);
        rt->PopAxisAlignedClip();

        if (!warning_.empty())
        {
            text(warning_, res_->small_text.Get(),
                 D2D1::RectF(32.0f, rc_license_.bottom + 10.0f, width - 32.0f,
                             rc_checkbox_.top - 6.0f),
                 theme_.error);
        }

        // Checkbox.
        stroke_round(rc_checkbox_, 4.0f, accepted_ ? theme_.accent : theme_.border);
        if (accepted_)
        {
            set(theme_.accent);
            rt->DrawLine(D2D1::Point2F(rc_checkbox_.left + 5.0f, rc_checkbox_.top + 10.0f),
                         D2D1::Point2F(rc_checkbox_.left + 8.5f, rc_checkbox_.top + 14.0f), brush,
                         2.0f);
            rt->DrawLine(D2D1::Point2F(rc_checkbox_.left + 8.5f, rc_checkbox_.top + 14.0f),
                         D2D1::Point2F(rc_checkbox_.left + 15.0f, rc_checkbox_.top + 6.0f), brush,
                         2.0f);
        }
        text(L"I accept the license terms", res_->small_text.Get(),
             D2D1::RectF(rc_checkbox_.right + 10.0f, rc_checkbox_.top + 1.0f,
                         rc_primary_.left - 12.0f, rc_checkbox_.bottom + 4.0f),
             theme_.text_body);

        // Primary button. Disabled until the box is ticked, and it looks it.
        const uint32_t edge = accepted_ ? (hover_primary_ ? theme_.accent : theme_.border_hover)
                                        : theme_.border;
        if (accepted_ && hover_primary_)
        {
            fill_round(rc_primary_, theme_.radius_button, theme_.accent_soft);
        }
        stroke_round(rc_primary_, theme_.radius_button, edge);
        tracked(L"INSTALL", res_->button.Get(), rc_primary_,
                accepted_ ? theme_.accent : theme_.text_faint, 0.10f, 12.0f);
        break;
    }

    case Page::Progress:
    {
        float fraction = 0.0f;
        std::wstring status;
        {
            std::lock_guard<std::mutex> lock(state_lock_);
            fraction = progress_;
            status = status_;
        }

        const D2D1_RECT_F track =
            D2D1::RectF(32.0f, height / 2.0f - 4.0f, width - 32.0f, height / 2.0f + 4.0f);
        fill_round(track, 4.0f, theme_.surface);
        stroke_round(track, 4.0f, theme_.border);

        if (fraction > 0.0f)
        {
            const D2D1_RECT_F bar = D2D1::RectF(
                track.left, track.top, track.left + (track.right - track.left) * fraction,
                track.bottom);
            fill_round(bar, 4.0f, theme_.accent);
        }

        text(status.empty() ? L"Preparing" : status, res_->small_text.Get(),
             D2D1::RectF(32.0f, height / 2.0f + 20.0f, width - 32.0f, height / 2.0f + 44.0f),
             theme_.text_muted);
        break;
    }

    case Page::Complete:
    {
        text(L"Installation complete.", res_->body.Get(),
             D2D1::RectF(32.0f, height / 2.0f - 20.0f, width - 32.0f, height / 2.0f + 8.0f),
             theme_.success);
        if (!install_dir_.empty())
        {
            text(install_dir_, res_->mono.Get(),
                 D2D1::RectF(32.0f, height / 2.0f + 12.0f, width - 32.0f, height / 2.0f + 36.0f),
                 theme_.text_faint);
        }
        stroke_round(rc_primary_, theme_.radius_button, theme_.border_hover);
        tracked(L"CLOSE", res_->button.Get(), rc_primary_, theme_.accent, 0.10f, 12.0f);
        break;
    }

    case Page::Error:
    {
        std::wstring message;
        {
            std::lock_guard<std::mutex> lock(state_lock_);
            message = error_;
        }
        text(L"Installation failed.", res_->body.Get(),
             D2D1::RectF(32.0f, 180.0f, width - 32.0f, 208.0f), theme_.error);
        text(message, res_->small_text.Get(),
             D2D1::RectF(32.0f, 212.0f, width - 32.0f, height - 90.0f), theme_.text_body);
        stroke_round(rc_primary_, theme_.radius_button, theme_.border_hover);
        tracked(L"CLOSE", res_->button.Get(), rc_primary_, theme_.text_body, 0.10f, 12.0f);
        break;
    }
    }

    // Footer.
    std::wstring footer = publisher_;
    if (!version_.empty())
    {
        footer += L"  ";
        footer += version_;
    }
    if (dev_build_)
    {
        footer += L"   UNSIGNED BUILD";
    }
    text(footer, res_->small_text.Get(),
         D2D1::RectF(32.0f, height - 20.0f, width - 32.0f, height - 2.0f),
         dev_build_ ? theme_.error : theme_.text_faint);

    const HRESULT hr = rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
    {
        // A driver reset or an RDP session change invalidates the target.
        // Without this the window goes permanently blank mid-install.
        discard_device_resources();
    }
}

} // namespace lwi::stub
