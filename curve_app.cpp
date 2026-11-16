#include "curve_chart.h"
#include "pwm_data.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <thread>

#ifdef _MSC_VER
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace {
using std::filesystem::path;

constexpr UINT work_done = WM_APP + 1;
constexpr UINT_PTR progress_timer = 1;
constexpr UINT_PTR frame_timer = 2;
constexpr int inset = 14;
constexpr int chart_top = 170;
constexpr int status_height = 37;
constexpr wchar_t class_name[] = L"PwmCsvCurveWindow";

enum ControlId {
    open_csv = 100, save_image, zoom_in, zoom_out, reset_view, pwm_precision, save_converted,
    x_column, x_unit, x_decimals, y_column, y_unit, y_decimals,
    data_mode, generate_plot, pwm_polarity, pwm_time_unit, pwm_threshold,
};

struct Unit {
    const wchar_t* option;
    const wchar_t* suffix;
    double factor;
};

constexpr Unit pwm_display_units[] = {
    {L"秒 (s)", L"s", 1.0},
    {L"分钟 (min)", L"min", 1.0 / 60.0},
    {L"小时 (h)", L"h", 1.0 / 3600.0},
};

constexpr Unit units[] = {
    {L"原始值", L"", 1.0},
    {L"ns → µs", L"µs", 1e-3}, {L"ns → ms", L"ms", 1e-6}, {L"ns → s", L"s", 1e-9},
    {L"µs → ms", L"ms", 1e-3}, {L"µs → s", L"s", 1e-6}, {L"ms → s", L"s", 1e-3},
    {L"% → 比例", L"比例", 1e-2}, {L"比例 → %", L"%", 1e2},
    {L"× 1,000", L"× 1,000", 1e3}, {L"÷ 1,000", L"÷ 1,000", 1e-3},
    {L"秒 → 分钟", L"min", pwm_display_units[1].factor},
    {L"秒 → 小时", L"h", pwm_display_units[2].factor},
};

struct AxisControls {
    HWND label = nullptr;
    HWND column = nullptr;
    HWND unit_label = nullptr;
    HWND unit = nullptr;
    HWND decimals_label = nullptr;
    HWND decimals = nullptr;
};

enum class Work { none, loading, generating, rendering, saving };
enum class Dialog { open_csv, jpeg, save_csv };

struct FrameSize {
    int width = 0;
    int height = 0;
    float scale = 1.0f;
};

struct CursorReadout {
    POINT point;
    RECT area;
    RECT box;
    std::wstring x;
    std::wstring y;
};

struct GeneratedChart {
    std::optional<PwmSeries> pwm;
    Plot plot;
};

constexpr double seconds_per_unit[] = {1.0, 1e-3, 1e-6, 1e-9};

std::wstring lower_text(std::wstring text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return text;
}

std::wstring column_unit(const Column& column)
{
    if (column.percent || column.name.find(L'%') != std::wstring::npos) {
        return L"%";
    }
    const auto& name = column.name;
    const auto last = name.find_last_not_of(L" \t");
    if (last == std::wstring::npos) {
        return {};
    }
    const wchar_t closing = name[last];
    if (closing == L']' || closing == L')' || closing == L'）') {
        const wchar_t opening = closing == L']' ? L'[' : closing == L')' ? L'(' : L'（';
        const auto first = name.find_last_of(opening, last);
        if (first != std::wstring::npos && first + 1 < last) {
            return name.substr(first + 1, last - first - 1);
        }
    }
    const auto separator = name.find_last_of(L"_ ", last);
    if (separator != std::wstring::npos) {
        const auto suffix = name.substr(separator + 1, last - separator);
        for (const auto* unit : {L"s", L"ms", L"us", L"µs", L"μs", L"ns", L"min", L"h", L"V", L"mV",
                                 L"A", L"mA", L"Hz", L"kHz", L"MHz", L"rpm", L"mm", L"cm",
                                 L"m", L"km", L"m/s", L"km/h", L"°C", L"°F", L"deg", L"rad"}) {
            if (suffix == unit) return suffix;
        }
    }
    return {};
}

int time_unit_for(const Column* column)
{
    if (!column) return 0;
    const auto name = lower_text(column->name);
    if (name.find(L"纳秒") != name.npos || name.find(L"[ns]") != name.npos ||
        name.find(L"(ns)") != name.npos || name.find(L"_ns") != name.npos) return 3;
    if (name.find(L"微秒") != name.npos || name.find(L"[us]") != name.npos ||
        name.find(L"[µs]") != name.npos || name.find(L"[μs]") != name.npos ||
        name.find(L"(us)") != name.npos || name.find(L"_us") != name.npos) return 2;
    if (name.find(L"毫秒") != name.npos || name.find(L"[ms]") != name.npos ||
        name.find(L"(ms)") != name.npos || name.find(L"_ms") != name.npos) return 1;
    return 0;
}

std::wstring formatted_count(std::size_t count)
{
    std::wstring result = std::to_wstring(count);
    for (int i = static_cast<int>(result.size()) - 3; i > 0; i -= 3) {
        result.insert(static_cast<std::size_t>(i), 1, L',');
    }
    return result;
}

std::wstring file_dialog(HWND parent, Dialog kind, const std::wstring& suggested = {})
{
    wchar_t filename[32768] = {};
    suggested.copy(filename, std::min(suggested.size(), std::size(filename) - 1));
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = parent;
    dialog.lpstrFile = filename;
    dialog.nMaxFile = static_cast<DWORD>(std::size(filename));
    const bool save = kind != Dialog::open_csv;
    const bool jpeg = kind == Dialog::jpeg;
    dialog.lpstrFilter = jpeg ? L"JPEG 图片 (*.jpg;*.jpeg)\0*.jpg;*.jpeg\0"
                              : L"CSV 文件 (*.csv)\0*.csv\0";
    dialog.lpstrTitle = jpeg ? L"保存当前视图为 JPG" : save ? L"保存转换后的 CSV" : L"打开 CSV";
    dialog.lpstrDefExt = jpeg ? L"jpg" : L"csv";
    dialog.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST |
                   (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return (save ? GetSaveFileNameW(&dialog) : GetOpenFileNameW(&dialog)) ? filename : L"";
}

int selection(HWND combo)
{
    return static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
}

void select(HWND combo, int index)
{
    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
}

LRESULT add_option(HWND combo, const wchar_t* label)
{
    return SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
}

double upper_bound(double max_data)
{
    if (max_data == 0.0) {
        return 1.0;
    }
    const double padded = max_data <= std::numeric_limits<double>::max() / 1.05
                            ? max_data * 1.05 : max_data;
    return std::max(padded, 1e-290);
}

Range bounded_range(double low, double span, const Range& full)
{
    span = std::clamp(span, full.span() * 1e-12, full.span());
    low = std::clamp(low, full.low, full.high - span);
    return {low, low + span};
}

Range zoom_range(const Range& current, const Range& full, double factor, double fraction)
{
    const double span = std::clamp(current.span() * factor, full.span() * 1e-12, full.span());
    return bounded_range(current.low + (current.span() - span) * fraction, span, full);
}

void fit_chart(Plot& plot, bool pwm, const std::atomic_bool& cancel)
{
    double max_x = 0.0;
    double max_y = 0.0;
    const auto& x = plot.x->values;
    const auto& y = plot.y->values;
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (i % 4096 == 0 && cancel.load(std::memory_order_relaxed)) return;
        if (x[i] >= 0.0 && y[i] >= 0.0) {
            max_x = std::max(max_x, x[i]);
            max_y = std::max(max_y, y[i]);
            ++plot.point_count;
        }
    }
    plot.view = {{0.0, upper_bound(max_x)}, {0.0, pwm ? 100.0 : upper_bound(max_y)}};
}

class Window {
public:
    HWND handle = nullptr;

    void initialize();
    void layout();
    void paint();
    void command(int id, int notification);
    void drop(HDROP files);
    void timer(UINT_PTR id);
    void finish_work();
    void wheel(POINT screen_point, int delta);
    void pointer_moved(POINT client_point);
    void pointer_left();
    void begin_pan(POINT client_point);
    void end_pan();
    void close();
    HBRUSH label_background(HDC dc) const;

    void load(const path& filename);

    void set_minimum(MINMAXINFO& info) const { info.ptMinTrackSize = {px(815), px(450)}; }

private:
    HWND open_ = nullptr;
    HWND save_ = nullptr;
    HWND zoom_in_ = nullptr;
    HWND zoom_out_ = nullptr;
    HWND reset_ = nullptr;
    HWND mode_label_ = nullptr;
    HWND mode_ = nullptr;
    HWND generate_ = nullptr;
    HWND polarity_label_ = nullptr;
    HWND polarity_ = nullptr;
    HWND time_label_ = nullptr;
    HWND time_unit_ = nullptr;
    HWND threshold_label_ = nullptr;
    HWND threshold_ = nullptr;
    HWND pwm_label_ = nullptr;
    HWND pwm_precision_ = nullptr;
    HWND save_csv_ = nullptr;
    HWND status_ = nullptr;
    AxisControls x_;
    AxisControls y_;
    HFONT font_ = nullptr;
    HBRUSH background_ = nullptr;
    HPEN guide_pen_ = nullptr;
    float scale_ = 1.0f;
    CsvTable table_;
    std::shared_ptr<GeneratedChart> chart_;
    View requested_;
    std::shared_ptr<Gdiplus::Bitmap> image_;
    FrameSize display_frame_;
    View display_view_;
    FrameSize pending_frame_;
    View pending_view_;
    HDC display_dc_ = nullptr;
    HBITMAP display_bitmap_ = nullptr;
    SIZE display_size_ = {};
    bool tracking_pointer_ = false;
    std::optional<POINT> pointer_;
    std::optional<CursorReadout> readout_;
    bool frame_pending_ = false;
    bool settings_dirty_ = false;
    bool pwm_display_units_active_ = false;
    bool choosing_csv_output_ = false;
    bool panning_ = false;
    Gdiplus::PointF pan_anchor_;
    View pan_start_;
    Work work_ = Work::none;
    path work_path_;
    std::thread worker_;
    std::atomic_bool cancel_ = false;
    std::atomic_size_t progress_ = 0;
    CsvTable pending_table_;
    std::shared_ptr<GeneratedChart> pending_chart_;
    std::shared_ptr<Gdiplus::Bitmap> pending_image_;
    std::wstring error_;

    int px(int dips) const { return static_cast<int>(std::lround(static_cast<float>(dips) * scale_)); }
    float dip(int pixels) const { return static_cast<float>(pixels) / scale_; }
    HWND control(const wchar_t* type, const wchar_t* title, DWORD style, int id = 0) const;
    HWND button(const wchar_t* title, int id) const;
    HWND label(const wchar_t* title) const;
    HWND combo(int id) const;
    void create_axis(AxisControls& controls, const wchar_t* axis, int col, int unit, int decimals);
    void place(HWND child, int x, int y, int width, int height) const;
    void layout_axis(const AxisControls& axis, int width, int top) const;
    RECT chart_bounds() const;
    Gdiplus::PointF chart_position(POINT point) const;
    Gdiplus::RectF chart_area() const;
    std::wstring chart_title() const { return table_.path.filename().wstring(); }
    void repaint_chart() const;
    FrameSize frame_size() const;
    void publish_frame(const View& view);
    void clear_display_cache();
    void paint_cached_chart(HDC dc, const RECT& rect);
    void refresh_readout();
    void invalidate_readout(const CursorReadout& readout) const;
    void paint_readout(HDC dc) const;
    void pan(POINT client_point);
    void request_frame(UINT delay = 140);
    void render_view();
    void zoom(double factor, double x, double y);
    void set_status(const std::wstring& text) const { SetWindowTextW(status_, text.c_str()); }
    void update_status() const;
    void enable_controls() const;
    void fill_axis_choices();
    void update_x_units(bool pwm);
    const Column* column_for(const AxisControls& axis) const;
    AxisStyle style_for(const AxisControls& controls, const Column* column) const;
    bool pwm_mode() const { return selection(mode_) == 1; }
    bool can_navigate() const;
    void settings_changed();
    void report_progress();
    template<class Job> bool start_work(Work kind, Job job);
    void generate();
    void export_csv();
    void export_image() const;
};

void Window::initialize()
{
    const HDC dc = GetDC(handle);
    scale_ = static_cast<float>(GetDeviceCaps(dc, LOGPIXELSX)) / 96.0f;
    ReleaseDC(handle, dc);
    font_ = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    background_ = CreateSolidBrush(RGB(242, 246, 250));
    guide_pen_ = CreatePen(PS_DOT, 1, RGB(58, 103, 149));
    open_ = button(L"打开 CSV…", open_csv);
    mode_label_ = label(L"数据模式");
    mode_ = combo(data_mode);
    add_option(mode_, L"常规数据");
    add_option(mode_, L"原始 PWM 方波数据");
    select(mode_, 0);
    generate_ = button(L"生成曲线", generate_plot);
    save_ = button(L"保存 JPG…", save_image);
    zoom_out_ = button(L"−", zoom_out);
    zoom_in_ = button(L"+", zoom_in);
    reset_ = button(L"全图", reset_view);
    create_axis(x_, L"X 轴列", x_column, x_unit, x_decimals);
    create_axis(y_, L"Y 轴列", y_column, y_unit, y_decimals);
    threshold_label_ = label(L"电平阈值");
    threshold_ = control(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, pwm_threshold);
    SendMessageW(threshold_, EM_SETLIMITTEXT, 40, 0);
    SendMessageW(threshold_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"自动"));
    polarity_label_ = label(L"PWM 有效电平");
    polarity_ = combo(pwm_polarity);
    add_option(polarity_, L"高有效");
    add_option(polarity_, L"低有效");
    select(polarity_, 0);
    time_label_ = label(L"输入时间单位");
    time_unit_ = combo(pwm_time_unit);
    for (const auto* unit : {L"秒 (s)", L"毫秒 (ms)", L"微秒 (µs)", L"纳秒 (ns)"}) {
        add_option(time_unit_, unit);
    }
    select(time_unit_, 0);
    pwm_label_ = label(L"PWM 分组");
    pwm_precision_ = combo(pwm_precision);
    add_option(pwm_precision_, L"1 秒");
    for (int digits = 1; digits <= 9; ++digits) {
        const auto option = L"0." + std::wstring(digits - 1, L'0') + L"1 秒";
        add_option(pwm_precision_, option.c_str());
    }
    select(pwm_precision_, 1);
    save_csv_ = button(L"保存转换 CSV…", save_converted);
    status_ = label(L"");
    DragAcceptFiles(handle, TRUE);
    enable_controls();
}

HWND Window::control(const wchar_t* type, const wchar_t* title, DWORD style, int id) const
{
    HWND child = CreateWindowExW(0, type, title, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                                 handle, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 GetModuleHandleW(nullptr), nullptr);
    SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    return child;
}

HWND Window::button(const wchar_t* title, int id) const
{
    return control(L"BUTTON", title, BS_PUSHBUTTON | WS_TABSTOP, id);
}

HWND Window::label(const wchar_t* title) const
{
    return control(L"STATIC", title, SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS);
}

HWND Window::combo(int id) const
{
    return control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, id);
}

void Window::create_axis(AxisControls& axis, const wchar_t* title, int col, int unit, int decimals)
{
    axis.label = label(title);
    axis.column = combo(col);
    axis.unit_label = label(L"单位换算");
    axis.unit = combo(unit);
    for (const auto& entry : units) {
        add_option(axis.unit, entry.option);
    }
    select(axis.unit, 0);
    axis.decimals_label = label(L"刻度小数位");
    axis.decimals = combo(decimals);
    add_option(axis.decimals, L"自动");
    for (int digits = 0; digits <= 9; ++digits) {
        add_option(axis.decimals, std::to_wstring(digits).c_str());
    }
    select(axis.decimals, 0);
}

void Window::place(HWND child, int x, int y, int width, int height) const
{
    MoveWindow(child, px(x), px(y), px(width), px(height), TRUE);
}

void Window::layout_axis(const AxisControls& axis, int width, int top) const
{
    const int unit = width - 370;
    place(axis.label, inset, top - 2, 52, 27);
    place(axis.column, 67, top, std::max(40, unit - 82), 360);
    place(axis.unit_label, unit, top - 2, 66, 27);
    place(axis.unit, unit + 68, top, 112, 360);
    place(axis.decimals_label, unit + 193, top - 2, 81, 27);
    place(axis.decimals, unit + 276, top, 80, 360);
}

void Window::layout()
{
    RECT bounds = {};
    GetClientRect(handle, &bounds);
    const int width = static_cast<int>(dip(bounds.right));
    place(open_, inset, 10, 107, 32);
    place(mode_label_, 136, 10, 65, 32);
    place(mode_, 202, 13, 149, 155);
    place(generate_, 362, 10, 97, 32);
    place(save_, 470, 10, 108, 32);
    place(zoom_out_, 593, 10, 35, 32);
    place(zoom_in_, 634, 10, 35, 32);
    place(reset_, 675, 10, 62, 32);
    layout_axis(x_, width, 57);
    layout_axis(y_, width, 93);
    place(threshold_label_, width - 370, 91, 66, 27);
    place(threshold_, width - 302, 93, 112, 25);
    place(polarity_label_, inset, 126, 102, 28);
    place(polarity_, 116, 129, 88, 155);
    place(time_label_, 221, 126, 90, 28);
    place(time_unit_, 312, 129, 96, 195);
    place(pwm_label_, 425, 126, 72, 28);
    place(pwm_precision_, 498, 129, 120, 340);
    place(save_csv_, 629, 126, 133, 30);
    MoveWindow(status_, px(inset), bounds.bottom - px(status_height - 3),
               std::max(1L, bounds.right - px(inset * 2)), px(status_height - 6), TRUE);
    request_frame(190);
    refresh_readout();
    InvalidateRect(handle, nullptr, FALSE);
}

RECT Window::chart_bounds() const
{
    RECT rect = {};
    GetClientRect(handle, &rect);
    return {px(inset), px(chart_top), rect.right - px(inset), rect.bottom - px(status_height)};
}

Gdiplus::PointF Window::chart_position(POINT point) const
{
    const auto rect = chart_bounds();
    return {dip(point.x - rect.left), dip(point.y - rect.top)};
}

Gdiplus::RectF Window::chart_area() const
{
    const auto rect = chart_bounds();
    const float width = dip(rect.right - rect.left);
    const float height = dip(rect.bottom - rect.top);
    if (!image_) {
        return plot_area(width, height);
    }
    const float frame_width = static_cast<float>(display_frame_.width) / display_frame_.scale;
    const float frame_height = static_cast<float>(display_frame_.height) / display_frame_.scale;
    const auto area = plot_area(frame_width, frame_height);
    const float horizontal = width / frame_width;
    const float vertical = height / frame_height;
    return {area.X * horizontal, area.Y * vertical,
            area.Width * horizontal, area.Height * vertical};
}

void Window::repaint_chart() const
{
    const auto rect = chart_bounds();
    InvalidateRect(handle, &rect, FALSE);
}

FrameSize Window::frame_size() const
{
    const auto rect = chart_bounds();
    const float scale = std::max(1.5f, scale_);
    const float width = std::max(dip(rect.right - rect.left), 200.0f);
    const float height = std::max(dip(rect.bottom - rect.top), 180.0f);
    return {static_cast<int>(std::lround(width * scale)), static_cast<int>(std::lround(height * scale)), scale};
}

void Window::publish_frame(const View& view)
{
    clear_display_cache();
    display_frame_ = pending_frame_;
    display_view_ = view;
    image_ = std::move(pending_image_);
    refresh_readout();
    repaint_chart();
}

void Window::clear_display_cache()
{
    if (display_dc_) {
        DeleteDC(display_dc_);
        display_dc_ = nullptr;
    }
    if (display_bitmap_) {
        DeleteObject(display_bitmap_);
        display_bitmap_ = nullptr;
    }
}

void Window::paint_cached_chart(HDC dc, const RECT& rect)
{
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (!display_dc_ || display_size_.cx != width || display_size_.cy != height) {
        clear_display_cache();
        display_dc_ = CreateCompatibleDC(dc);
        display_bitmap_ = CreateCompatibleBitmap(dc, width, height);
        if (display_dc_ && display_bitmap_ && SelectObject(display_dc_, display_bitmap_)) {
            Gdiplus::Status status;
            {
                Gdiplus::Graphics graphics(display_dc_);
                graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
                status = graphics.DrawImage(image_.get(), 0, 0, width, height);
            }
            if (status == Gdiplus::Ok) {
                display_size_ = {width, height};
            } else {
                clear_display_cache();
            }
        } else {
            clear_display_cache();
        }
    }
    if (display_dc_) {
        BitBlt(dc, rect.left, rect.top, width, height, display_dc_, 0, 0, SRCCOPY);
    } else {
        Gdiplus::Graphics graphics(dc);
        graphics.DrawImage(image_.get(), rect.left, rect.top, width, height);
    }
}

void Window::invalidate_readout(const CursorReadout& readout) const
{
    const auto& area = readout.area;
    const auto& point = readout.point;
    const RECT vertical{point.x, area.top, point.x + 1, area.bottom};
    const RECT horizontal{area.left, point.y, area.right, point.y + 1};
    InvalidateRect(handle, &vertical, FALSE);
    InvalidateRect(handle, &horizontal, FALSE);
    InvalidateRect(handle, &readout.box, FALSE);
}

void Window::refresh_readout()
{
    if (readout_) {
        invalidate_readout(*readout_);
        readout_.reset();
    }
    if (!pointer_ || !image_ || !chart_ || panning_) {
        return;
    }
    const auto point = *pointer_;
    const auto cursor = chart_position(point);
    const auto area = chart_area();
    if (!area.Contains(cursor)) {
        return;
    }
    const auto bounds = chart_bounds();
    const RECT pixels{
        bounds.left + std::lround(area.X * scale_),
        bounds.top + std::lround(area.Y * scale_),
        bounds.left + std::lround(area.GetRight() * scale_),
        bounds.top + std::lround(area.GetBottom() * scale_),
    };
    const auto& plot = chart_->plot;
    CursorReadout readout{point, pixels, {},
        L"X  " + coordinate_label(display_view_.x, plot.x_style,
                                    (cursor.X - area.X) / area.Width, area.Width * scale_),
        L"Y  " + coordinate_label(display_view_.y, plot.y_style,
                                    (area.GetBottom() - cursor.Y) / area.Height, area.Height * scale_),
    };
    const LONG gap = px(12);
    LONG width = px(120);
    if (HDC dc = GetDC(handle)) {
        const auto previous = SelectObject(dc, font_);
        SIZE text_size{};
        for (const auto* text : {&readout.x, &readout.y}) {
            if (GetTextExtentPoint32W(dc, text->c_str(), static_cast<int>(text->size()), &text_size)) {
                width = std::max(width, text_size.cx + gap * 2);
            }
        }
        SelectObject(dc, previous);
        ReleaseDC(handle, dc);
    }
    width = std::min(width, pixels.right - pixels.left);
    const LONG height = std::min(static_cast<LONG>(px(54)), pixels.bottom - pixels.top);
    const LONG left = std::clamp(point.x + gap + width <= pixels.right ? point.x + gap : point.x - gap - width,
                                 pixels.left, pixels.right - width);
    const LONG top = std::clamp(point.y + gap + height <= pixels.bottom ? point.y + gap : point.y - gap - height,
                                pixels.top, pixels.bottom - height);
    readout.box = {left, top, left + width, top + height};
    readout_ = std::move(readout);
    invalidate_readout(*readout_);
}

void Window::paint_readout(HDC dc) const
{
    if (!readout_) return;
    const auto& readout = *readout_;
    const auto& area = readout.area;
    const auto& point = readout.point;
    const auto saved = SaveDC(dc);
    if (saved == 0) return;
    IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, guide_pen_);
    MoveToEx(dc, point.x, area.top, nullptr);
    LineTo(dc, point.x, area.bottom);
    MoveToEx(dc, area.left, point.y, nullptr);
    LineTo(dc, area.right, point.y);

    FillRect(dc, &readout.box, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    FrameRect(dc, &readout.box, static_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH)));
    SetTextColor(dc, RGB(24, 43, 59));
    SelectObject(dc, font_);
    RECT row = readout.box;
    InflateRect(&row, -px(10), -px(4));
    const LONG middle = (row.top + row.bottom) / 2;
    RECT first{row.left, row.top, row.right, middle};
    RECT second{row.left, middle, row.right, row.bottom};
    constexpr UINT format = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS;
    DrawTextW(dc, readout.x.c_str(), -1, &first, format);
    DrawTextW(dc, readout.y.c_str(), -1, &second, format);
    RestoreDC(dc, saved);
}

void Window::paint()
{
    PAINTSTRUCT paint_info = {};
    HDC dc = BeginPaint(handle, &paint_info);
    FillRect(dc, &paint_info.rcPaint, background_);
    auto rect = chart_bounds();
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width > 0 && height > 0 && image_) {
        paint_cached_chart(dc, rect);
        paint_readout(dc);
    } else if (width > 0 && height > 0) {
        FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        const auto previous = SelectObject(dc, font_);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(83, 105, 122));
        const wchar_t* hint = work_ == Work::generating ? L"正在生成曲线…" :
                              work_ == Work::loading ? L"正在读取 CSV…" :
                              table_.rows ? L"选择数据模式和列后，点击「生成曲线」" :
                                            L"拖入 CSV 文件，或点击「打开 CSV」";
        DrawTextW(dc, hint, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, previous);
    }
    EndPaint(handle, &paint_info);
}

HBRUSH Window::label_background(HDC dc) const
{
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(24, 43, 59));
    return background_;
}

void Window::update_status() const
{
    if (table_.rows == 0) {
        set_status(L"");
        return;
    }
    std::wstring status = table_.path.filename().wstring() + L"   ·   " +
                          formatted_count(table_.rows) + L" 行";
    if (chart_ && chart_->pwm) {
        const auto& pwm = *chart_->pwm;
        status += L"   ·   PWM " + formatted_count(pwm.time.values.size()) + L" 组";
        if (pwm.repaired > 0) {
            status += L"   ·   合并 " + formatted_count(pwm.repaired) + L" 对短脉冲";
        }
    }
    if (settings_dirty_ || !chart_) {
        status += L"   ·   点击「生成曲线」应用设置";
    } else {
        status += L"   ·   滚轮缩放，拖动后松开平移";
    }
    set_status(status);
}

void Window::enable_controls() const
{
    const bool idle = work_ == Work::none;
    const bool editable = table_.rows && (idle || work_ == Work::rendering);
    const bool pwm = pwm_mode();
    const auto* signal = column_for(y_);
    const bool uses_threshold = pwm && signal && !duty_column(*signal);
    ShowWindow(y_.unit_label, uses_threshold ? SW_HIDE : SW_SHOWNA);
    ShowWindow(y_.unit, uses_threshold ? SW_HIDE : SW_SHOWNA);
    ShowWindow(threshold_label_, uses_threshold ? SW_SHOWNA : SW_HIDE);
    ShowWindow(threshold_, uses_threshold ? SW_SHOWNA : SW_HIDE);
    EnableWindow(threshold_, editable && uses_threshold);
    EnableWindow(open_, idle);
    EnableWindow(mode_, editable);
    EnableWindow(generate_, table_.rows && idle);
    EnableWindow(polarity_, editable && pwm);
    EnableWindow(time_unit_, editable && pwm);
    EnableWindow(pwm_precision_, editable && pwm);
    EnableWindow(save_csv_, chart_ && chart_->pwm && idle);
    EnableWindow(save_, image_ && (idle || work_ == Work::rendering));
    EnableWindow(reset_, can_navigate());
    EnableWindow(zoom_in_, can_navigate());
    EnableWindow(zoom_out_, can_navigate());
    EnableWindow(x_.unit, editable);
    EnableWindow(y_.unit, editable && !pwm);
    for (const AxisControls* axis : {&x_, &y_}) {
        EnableWindow(axis->column, editable);
        EnableWindow(axis->decimals, editable);
    }
}

void Window::fill_axis_choices()
{
    SendMessageW(x_.column, CB_RESETCONTENT, 0, 0);
    SendMessageW(y_.column, CB_RESETCONTENT, 0, 0);
    int x_default = 0;
    int y_default = -1;
    int count = 0;
    for (std::size_t index = 0; index < table_.columns.size(); ++index) {
        const auto& column = table_.columns[index];
        if (column.values.empty()) {
            continue;
        }
        const auto name = std::to_wstring(index + 1) + L"  " + column.name;
        for (HWND combo_box : {x_.column, y_.column}) {
            const LRESULT item = add_option(combo_box, name.c_str());
            SendMessageW(combo_box, CB_SETITEMDATA, static_cast<WPARAM>(item),
                         static_cast<LPARAM>(index));
        }
        const auto column_name = lower_text(column.name);
        if (column_name.find(L"time") == 0 || column_name.find(L"时间") == 0) {
            x_default = count;
        }
        if (duty_column(column) || (y_default < 0 && (column_name.find(L"pwm") == 0 ||
                                                     column_name.find(L"level") == 0))) {
            y_default = count;
        }
        ++count;
    }
    if (y_default < 0) {
        y_default = count > 1 && x_default == 0 ? 1 : 0;
    }
    select(x_.column, x_default);
    select(y_.column, y_default);
    select(x_.unit, 0);
    select(y_.unit, 0);
    SetWindowTextW(threshold_, L"");
    select(time_unit_, time_unit_for(column_for(x_)));
    const Column* time = column_for(x_);
    const Column* signal = column_for(y_);
    const auto time_name = time ? lower_text(time->name) : L"";
    const auto signal_name = signal ? lower_text(signal->name) : L"";
    const bool has_time = time_name.find(L"time") == 0 || time_name.find(L"时间") == 0;
    const bool looks_like_pwm = signal_name.find(L"pwm") != signal_name.npos ||
                                signal_name.find(L"duty") != signal_name.npos ||
                                signal_name.find(L"占空比") != signal_name.npos;
    select(mode_, signal && time != signal && has_time && looks_like_pwm && signal_name != L"duty_%" ? 1 : 0);
}

void Window::update_x_units(bool pwm)
{
    if (pwm == pwm_display_units_active_) {
        return;
    }
    pwm_display_units_active_ = pwm;
    SendMessageW(x_.unit, CB_RESETCONTENT, 0, 0);
    if (pwm) {
        for (const auto& unit : pwm_display_units) {
            add_option(x_.unit, unit.option);
        }
    } else {
        for (const auto& unit : units) {
            add_option(x_.unit, unit.option);
        }
    }
    select(x_.unit, 0);
}

const Column* Window::column_for(const AxisControls& axis) const
{
    const int selected = selection(axis.column);
    if (selected < 0) {
        return nullptr;
    }
    const auto index = static_cast<std::size_t>(
        SendMessageW(axis.column, CB_GETITEMDATA, static_cast<WPARAM>(selected), 0));
    return index < table_.columns.size() ? &table_.columns[index] : nullptr;
}

AxisStyle Window::style_for(const AxisControls& controls, const Column* column) const
{
    if (!column) {
        return {};
    }
    const auto unit_index = std::max(0, selection(controls.unit));
    const auto decimals = selection(controls.decimals) - 1;
    if (&controls == &x_ && pwm_display_units_active_) {
        const auto& unit = pwm_display_units[unit_index];
        return {L"时间 [" + std::wstring(unit.suffix) + L"]", unit.factor, decimals, unit.suffix};
    }
    const auto& unit = units[unit_index];
    std::wstring name = column->name;
    std::wstring coordinate_unit = column_unit(*column);
    if (unit.suffix[0]) {
        name += L"  [" + std::wstring(unit.suffix) + L"]";
        coordinate_unit = unit.suffix[0] == L'×' || unit.suffix[0] == L'÷' ?
                          L"" : unit.suffix;
    }
    return {std::move(name), unit.factor, decimals, std::move(coordinate_unit)};
}

void Window::settings_changed()
{
    settings_dirty_ = true;
    const bool pwm = pwm_mode();
    update_x_units(pwm);
    SetWindowTextW(x_.label, pwm ? L"时间列" : L"X 轴列");
    SetWindowTextW(y_.label, pwm ? L"信号列" : L"Y 轴列");
    SetWindowTextW(x_.unit_label, pwm ? L"显示单位" : L"单位换算");
    update_status();
    enable_controls();
}

template<class Job>
bool Window::start_work(Work kind, Job job)
{
    if (work_ != Work::none || worker_.joinable()) return false;
    work_ = kind;
    error_.clear();
    cancel_.store(false, std::memory_order_relaxed);
    try {
        worker_ = std::thread([this, job = std::move(job)] {
            try {
                job();
            } catch (const std::bad_alloc&) {
                error_ = L"可用内存不足，无法处理此 CSV。";
            } catch (const std::exception& error) {
                error_ = wide_text(error.what());
            }
            PostMessageW(handle, work_done, 0, 0);
        });
    } catch (const std::exception&) {
        work_ = Work::none;
        MessageBoxW(handle, L"无法启动后台任务。", L"数据曲线", MB_OK | MB_ICONERROR);
        return false;
    }
    enable_controls();
    return true;
}

void Window::load(const path& filename)
{
    if (work_ != Work::none || choosing_csv_output_) return;
    work_path_ = filename;
    progress_.store(0, std::memory_order_relaxed);
    if (start_work(Work::loading, [this, filename] {
        pending_table_ = load_table(filename, cancel_, progress_);
    })) {
        report_progress();
        SetTimer(handle, progress_timer, 200, nullptr);
        repaint_chart();
    }
}

void Window::generate()
{
    if (work_ != Work::none || !table_.rows) return;
    const auto* x = column_for(x_);
    const auto* y = column_for(y_);
    if (!x || !y) return;

    const bool pwm = pwm_mode();
    PwmOptions options{seconds_per_unit[std::max(selection(time_unit_), 0)],
                       selection(pwm_precision_), selection(polarity_) == 0, std::nullopt};
    if (pwm && !duty_column(*y)) {
        wchar_t input[41] = {};
        GetWindowTextW(threshold_, input, static_cast<int>(std::size(input)));
        const wchar_t* start = input;
        while (std::iswspace(*start)) ++start;
        if (*start) {
            wchar_t* end = nullptr;
            const double value = std::wcstod(start, &end);
            if (end != start) {
                while (std::iswspace(*end)) ++end;
            }
            if (end == start || *end || !std::isfinite(value)) {
                MessageBoxW(handle, L"请输入数字电平阈值，或留空使用自动阈值。", L"PWM 转换",
                            MB_OK | MB_ICONINFORMATION);
                return;
            }
            options.threshold = value;
        }
    }
    const auto x_style = style_for(x_, x);
    const auto y_style = style_for(y_, y);
    const auto size = frame_size();
    pending_frame_ = size;
    const auto title = chart_title();
    frame_pending_ = false;
    KillTimer(handle, frame_timer);
    if (start_work(Work::generating, [this, x, y, pwm, options, x_style, y_style, size, title] {
        auto next = std::make_shared<GeneratedChart>();
        next->plot = {x, y, x_style, y_style, {}, 0};
        if (pwm) {
            next->pwm = pwm_series(*x, *y, options, cancel_);
            next->plot.x = &next->pwm->time;
            next->plot.y = &next->pwm->duty;
            next->plot.y_style = {next->pwm->duty.name, 1.0, y_style.decimals, L"%"};
        }
        fit_chart(next->plot, pwm, cancel_);
        if (!cancel_.load(std::memory_order_relaxed)) {
            pending_image_ = render_chart(size.width, size.height, size.scale, next->plot, title);
            pending_chart_ = std::move(next);
        }
    })) {
        set_status(pwm ? L"正在转换 PWM 并生成曲线…" : L"正在生成曲线…");
        repaint_chart();
    }
}

void Window::report_progress()
{
    if (work_ == Work::loading) {
        set_status(L"正在加载 " + work_path_.filename().wstring() + L"   ·   已读取 " +
                   formatted_count(progress_.load(std::memory_order_relaxed)) + L" 行");
    }
}

void Window::finish_work()
{
    if (!worker_.joinable()) {
        return;
    }
    worker_.join();
    KillTimer(handle, progress_timer);
    const Work done = work_;
    work_ = Work::none;
    if (!error_.empty()) {
        update_status();
        enable_controls();
        repaint_chart();
        MessageBoxW(handle, error_.c_str(), L"数据曲线", MB_OK | MB_ICONERROR);
        return;
    }
    if (done == Work::loading) {
        chart_.reset();
        image_.reset();
        clear_display_cache();
        refresh_readout();
        frame_pending_ = false;
        KillTimer(handle, frame_timer);
        table_ = std::move(pending_table_);
        fill_axis_choices();
        settings_changed();
        repaint_chart();
    } else if (done == Work::generating) {
        chart_ = std::move(pending_chart_);
        requested_ = chart_->plot.view;
        publish_frame(requested_);
        settings_dirty_ = false;
        update_status();
    } else if (done == Work::rendering) {
        if (!frame_pending_) {
            publish_frame(pending_view_);
        } else {
            pending_image_.reset();
        }
    } else if (done == Work::saving) {
        set_status(L"已保存转换后的 CSV");
    }
    if (frame_pending_ && !panning_) {
        request_frame(70);
    }
    enable_controls();
}

bool Window::can_navigate() const
{
    return chart_ && chart_->plot.point_count && (work_ == Work::none || work_ == Work::rendering);
}

void Window::request_frame(UINT delay)
{
    if (chart_ || work_ == Work::generating) {
        frame_pending_ = true;
        SetTimer(handle, frame_timer, delay, nullptr);
    }
}

void Window::render_view()
{
    if (!chart_ || !frame_pending_ || panning_ || choosing_csv_output_ ||
        work_ != Work::none || IsIconic(handle)) return;
    Plot plot = chart_->plot;
    plot.view = requested_;
    const auto size = frame_size();
    const auto title = chart_title();
    pending_frame_ = size;
    pending_view_ = plot.view;
    frame_pending_ = false;
    start_work(Work::rendering, [this, plot, size, title] {
        pending_image_ = render_chart(size.width, size.height, size.scale, plot, title);
    });
}

void Window::timer(UINT_PTR id)
{
    if (id == progress_timer) {
        report_progress();
    } else if (id == frame_timer) {
        KillTimer(handle, frame_timer);
        render_view();
    }
}

void Window::export_csv()
{
    if (!chart_ || !chart_->pwm || work_ != Work::none) return;
    const auto current = chart_;
    choosing_csv_output_ = true;
    const auto filename = file_dialog(handle, Dialog::save_csv, table_.path.stem().wstring() + L"_v0.csv");
    choosing_csv_output_ = false;
    if (frame_pending_) request_frame(70);
    if (filename.empty()) return;
    std::error_code error;
    if (std::filesystem::equivalent(filename, table_.path, error) ||
        _wcsicmp(path(filename).extension().c_str(), L".csv") != 0) {
        MessageBoxW(handle, L"请使用另一个以 .csv 结尾的文件名。", L"保存转换 CSV", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (start_work(Work::saving, [current, filename] { save_pwm_csv(filename, *current->pwm); })) {
        set_status(L"正在保存转换后的 CSV…");
    }
}

void Window::export_image() const
{
    const auto image = image_;
    if (!image) return;
    const auto filename = file_dialog(handle, Dialog::jpeg, table_.path.stem().wstring() + L".jpg");
    if (filename.empty()) {
        return;
    }
    const auto extension = path(filename).extension().wstring();
    if (_wcsicmp(extension.c_str(), L".jpg") != 0 && _wcsicmp(extension.c_str(), L".jpeg") != 0) {
        MessageBoxW(handle, L"请输入以 .jpg 或 .jpeg 结尾的文件名。", L"保存 JPG", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (save_jpeg(filename, *image)) {
        set_status(L"已保存 JPG   ·   " + filename);
    } else {
        MessageBoxW(handle, L"无法保存 JPG，请检查路径和文件访问权限。", L"保存 JPG",
                    MB_OK | MB_ICONERROR);
    }
}

void Window::command(int id, int notification)
{
    if (id == pwm_threshold && notification == EN_CHANGE) {
        settings_changed();
        return;
    }
    if (notification == CBN_SELCHANGE) {
        if (id == x_column || id == y_column) {
            if (id == x_column) {
                if (!pwm_mode()) select(x_.unit, 0);
                select(time_unit_, time_unit_for(column_for(x_)));
            } else {
                select(y_.unit, 0);
                SetWindowTextW(threshold_, L"");
            }
        }
        if (id == data_mode || id == x_column || id == y_column || id == x_unit || id == y_unit ||
            id == x_decimals || id == y_decimals || id == pwm_polarity ||
            id == pwm_precision || id == pwm_time_unit) {
            settings_changed();
        }
        return;
    }
    if (notification != BN_CLICKED) {
        return;
    }
    switch (id) {
    case open_csv: {
        const auto filename = file_dialog(handle, Dialog::open_csv);
        if (!filename.empty()) {
            load(filename);
        }
        break;
    }
    case generate_plot: generate(); break;
    case save_image: export_image(); break;
    case zoom_in: zoom(0.8, 0.5, 0.5); break;
    case zoom_out: zoom(1.25, 0.5, 0.5); break;
    case reset_view:
        if (can_navigate()) {
            requested_ = chart_->plot.view;
            request_frame(50);
        }
        break;
    case save_converted: export_csv(); break;
    }
}

void Window::drop(HDROP files)
{
    const UINT length = DragQueryFileW(files, 0, nullptr, 0);
    std::wstring filename(length + 1, L'\0');
    if (length != 0) {
        DragQueryFileW(files, 0, filename.data(), length + 1);
    }
    DragFinish(files);
    if (length != 0) {
        filename.resize(length);
        load(filename);
    }
}

void Window::zoom(double factor, double x, double y)
{
    if (!can_navigate()) return;
    const auto& full = chart_->plot.view;
    requested_.x = zoom_range(requested_.x, full.x, factor, x);
    requested_.y = zoom_range(requested_.y, full.y, factor, y);
    request_frame();
}

void Window::wheel(POINT screen_point, int delta)
{
    ScreenToClient(handle, &screen_point);
    const auto cursor = chart_position(screen_point);
    const auto area = chart_area();
    if (area.Contains(cursor)) {
        zoom(std::pow(0.8, static_cast<double>(delta) / WHEEL_DELTA),
             (cursor.X - area.X) / area.Width, (area.GetBottom() - cursor.Y) / area.Height);
    }
}

void Window::pointer_moved(POINT client_point)
{
    if (!tracking_pointer_) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, handle, 0};
        tracking_pointer_ = TrackMouseEvent(&track) != FALSE;
    }
    pointer_ = client_point;
    if (panning_) {
        pan(client_point);
    } else {
        refresh_readout();
    }
}

void Window::pointer_left()
{
    tracking_pointer_ = false;
    pointer_.reset();
    refresh_readout();
}

void Window::begin_pan(POINT client_point)
{
    if (can_navigate() && chart_area().Contains(chart_position(client_point))) {
        SetFocus(handle);
        SetCapture(handle);
        panning_ = true;
        pointer_ = client_point;
        refresh_readout();
        pan_anchor_ = chart_position(client_point);
        pan_start_ = requested_;
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    }
}

void Window::pan(POINT client_point)
{
    if (!panning_) {
        return;
    }
    const auto cursor = chart_position(client_point);
    const auto area = chart_area();
    const double dx = (pan_anchor_.X - cursor.X) / area.Width * pan_start_.x.span();
    const double dy = (cursor.Y - pan_anchor_.Y) / area.Height * pan_start_.y.span();
    const auto& full = chart_->plot.view;
    requested_.x = bounded_range(pan_start_.x.low + dx, pan_start_.x.span(), full.x);
    requested_.y = bounded_range(pan_start_.y.low + dy, pan_start_.y.span(), full.y);
}

void Window::end_pan()
{
    if (!panning_) return;
    panning_ = false;
    if (GetCapture() == handle) {
        ReleaseCapture();
    }
    refresh_readout();
    request_frame(50);
}

void Window::close()
{
    cancel_.store(true, std::memory_order_relaxed);
    if (worker_.joinable()) {
        worker_.join();
    }
    clear_display_cache();
    if (guide_pen_) {
        DeleteObject(guide_pen_);
    }
    if (font_) {
        DeleteObject(font_);
    }
    if (background_) {
        DeleteObject(background_);
    }
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE) {
        auto* window = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        window->handle = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    }
    auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!window) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    switch (message) {
    case WM_CREATE: window->initialize(); return 0;
    case WM_SIZE: window->layout(); return 0;
    case WM_GETMINMAXINFO: window->set_minimum(*reinterpret_cast<MINMAXINFO*>(lparam)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: window->paint(); return 0;
    case WM_CTLCOLORSTATIC: return reinterpret_cast<LRESULT>(window->label_background(reinterpret_cast<HDC>(wparam)));
    case WM_COMMAND: window->command(LOWORD(wparam), HIWORD(wparam)); return 0;
    case WM_DROPFILES: window->drop(reinterpret_cast<HDROP>(wparam)); return 0;
    case WM_TIMER: window->timer(wparam); return 0;
    case work_done: window->finish_work(); return 0;
    case WM_MOUSEWHEEL:
        window->wheel({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, GET_WHEEL_DELTA_WPARAM(wparam));
        return 0;
    case WM_LBUTTONDOWN: window->begin_pan({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}); return 0;
    case WM_MOUSEMOVE: window->pointer_moved({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}); return 0;
    case WM_MOUSELEAVE: window->pointer_left(); return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED: window->end_pan(); return 0;
    case WM_DESTROY: window->close(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    SetProcessDPIAware();
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR gdiplus_token = 0;
    if (Gdiplus::GdiplusStartup(&gdiplus_token, &input, nullptr) != Gdiplus::Ok) {
        MessageBoxW(nullptr, L"无法启动 Windows 绘图组件。", L"数据曲线", MB_OK | MB_ICONERROR);
        return 1;
    }
    int result = 1;
    {
        WNDCLASSW type = {};
        type.lpfnWndProc = window_proc;
        type.hInstance = instance;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        type.lpszClassName = class_name;
        Window window;
        if (RegisterClassW(&type)) {
            const int dpi = static_cast<int>(GetDpiForSystem());
            const int width = std::min(MulDiv(1080, dpi, 96), GetSystemMetrics(SM_CXSCREEN));
            const int height = std::min(MulDiv(760, dpi, 96), GetSystemMetrics(SM_CYSCREEN));
            HWND hwnd = CreateWindowExW(0, class_name, L"CSV 数据曲线",
                                        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                        CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                                        nullptr, nullptr, instance, &window);
            if (hwnd) {
                ShowWindow(hwnd, show);
                int argc = 0;
                if (auto* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
                    if (argc > 1) {
                        window.load(argv[1]);
                    }
                    LocalFree(argv);
                }
                MSG message = {};
                BOOL status = 0;
                while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
                    if (!IsDialogMessageW(hwnd, &message)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
                result = status == 0 ? static_cast<int>(message.wParam) : 1;
            }
        }
    }
    Gdiplus::GdiplusShutdown(gdiplus_token);
    return result;
}
