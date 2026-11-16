#include "curve_chart.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <limits>
#include <stdexcept>

namespace {
using namespace Gdiplus;

const Color ink(24, 43, 59);
const Color muted(83, 105, 122);
const Color grid(222, 231, 238);
const Color paper(255, 255, 255);
const Color blue(20, 103, 191);

void text(Graphics& canvas, const std::wstring& value, const Font& font, Color color,
          const RectF& bounds, StringAlignment alignment = StringAlignmentNear)
{
    SolidBrush brush(color);
    StringFormat format;
    format.SetAlignment(alignment);
    format.SetLineAlignment(StringAlignmentCenter);
    format.SetFormatFlags(StringFormatFlagsNoWrap);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    canvas.DrawString(value.c_str(), -1, &font, bounds, &format, &brush);
}

double nice_step(double minimum)
{
    const double power = std::pow(10.0, std::floor(std::log10(minimum)));
    for (const double factor : {1.0, 2.0, 2.5, 5.0}) {
        if (power * factor >= minimum) {
            return power * factor;
        }
    }
    return power * 10.0;
}

std::wstring tick_label(double value, double step, int decimals)
{
    wchar_t buffer[96] = {};
    const double magnitude = std::abs(value);
    const double exponent = std::floor(std::log10(step));
    if (magnitude >= 1e12 || (decimals < 0 && exponent < -9)) {
        const double digits = magnitude > 0.0 ? std::floor(std::log10(magnitude)) - exponent : 0;
        const int precision = decimals < 0 ? static_cast<int>(std::clamp(digits, 1.0, 9.0)) : decimals;
        std::swprintf(buffer, std::size(buffer), L"%.*e", precision, value);
    } else {
        const int automatic = static_cast<int>(std::clamp(-exponent + 1.0, 0.0, 10.0));
        std::swprintf(buffer, std::size(buffer), L"%.*f", decimals < 0 ? automatic : decimals, value);
        if (decimals < 0 && automatic > 0) {
            std::size_t size = std::wcslen(buffer);
            while (size && buffer[size - 1] == L'0') {
                buffer[--size] = L'\0';
            }
            if (size && buffer[size - 1] == L'.') {
                buffer[size - 1] = L'\0';
            }
        }
    }
    return buffer;
}

template<class Draw>
void ticks(const Range& range, const AxisStyle& style, int divisions, Draw draw)
{
    const double span = range.span() * style.multiplier;
    const double low = range.low * style.multiplier;
    const double high = range.high * style.multiplier;
    if (!(span > 0.0) || !std::isfinite(high)) {
        return;
    }
    double step = nice_step(span / divisions);
    if (style.decimals >= 0) {
        step = (std::max)(step, std::pow(10.0, -style.decimals));
    }
    const double first = std::ceil(low / step - 1e-9) * step;
    for (int index = 0; index <= divisions + 1; ++index) {
        const double value = first + index * step;
        if (value > high + span * 1e-9) {
            break;
        }
        const float position = static_cast<float>((value - low) / span);
        draw(position, tick_label(value, step, style.decimals));
    }
}

bool valid_point(double x, double y)
{
    return x >= 0.0 && y >= 0.0;
}

// A bucket keeps endpoints and extrema in input order, including narrow spikes.
struct Bucket {
    std::size_t first = 0;
    std::size_t last = 0;
    std::size_t low_x = 0;
    std::size_t high_x = 0;
    std::size_t low_y = 0;
    std::size_t high_y = 0;
};

void draw_curve(Graphics& canvas, const RectF& area, const Plot& plot)
{
    const auto& x = plot.x->values;
    const auto& y = plot.y->values;
    const bool ordered = plot.x->ascending;
    const Range& rx = plot.view.x;
    const Range& ry = plot.view.y;
    std::size_t first = 0;
    std::size_t last = x.size();
    if (ordered) {
        first = static_cast<std::size_t>(std::lower_bound(x.begin(), x.end(), rx.low) - x.begin());
        last = static_cast<std::size_t>(std::upper_bound(x.begin(), x.end(), rx.high) - x.begin());
        first -= first > 0;
        last += last < x.size();
    }
    const auto budget = static_cast<std::size_t>(area.Width) * 3;
    const std::size_t stride = (std::max)(std::size_t{1}, (last - first) / budget);
    const auto project_x = [&](double value) { return ((value - rx.low) / rx.span()) * area.Width; };
    const auto project_y = [&](double value) { return ((value - ry.low) / ry.span()) * area.Height; };
    const auto limit = [](double value) { return static_cast<float>(std::clamp(value, -32768.0, 32768.0)); };

    std::vector<PointF> line;
    line.reserve(budget * 2);
    Pen pen(blue, 1.5f);
    pen.SetLineJoin(LineJoinRound);
    const SolidBrush dot(blue);
    const auto flush_line = [&] {
        if (line.size() > 1) {
            canvas.DrawLines(&pen, line.data(), static_cast<INT>(line.size()));
        } else if (!line.empty()) {
            canvas.FillEllipse(&dot, line[0].X - 1.7f, line[0].Y - 1.7f, 3.4f, 3.4f);
        }
        line.clear();
    };

    Bucket bucket;
    long long current_key = -1;
    bool pending = false;
    const auto flush_bucket = [&] {
        if (!pending) {
            return;
        }
        std::array<std::size_t, 6> indices{
            bucket.first, bucket.low_x, bucket.high_x, bucket.low_y, bucket.high_y, bucket.last};
        std::sort(indices.begin(), indices.end());
        const auto end = std::unique(indices.begin(), indices.end());
        for (auto i = indices.begin(); i != end; ++i) {
            const PointF point(area.X + limit(project_x(x[*i])), area.GetBottom() - limit(project_y(y[*i])));
            if (line.empty() || line.back().X != point.X || line.back().Y != point.Y) {
                line.push_back(point);
            }
        }
        pending = false;
    };

    const auto saved = canvas.Save();
    canvas.SetClip(area);
    canvas.SetSmoothingMode(SmoothingModeAntiAlias);
    for (std::size_t i = first; i < last; ++i) {
        if (!valid_point(x[i], y[i])) {
            flush_bucket();
            flush_line();
            continue;
        }
        const long long key = ordered ? static_cast<long long>(std::floor(limit(project_x(x[i]))))
                                      : static_cast<long long>(i / stride);
        if (!pending || key != current_key) {
            flush_bucket();
            bucket = {i, i, i, i, i, i};
            current_key = key;
            pending = true;
        } else {
            bucket.last = i;
            if (x[i] < x[bucket.low_x]) bucket.low_x = i;
            if (x[i] > x[bucket.high_x]) bucket.high_x = i;
            if (y[i] < y[bucket.low_y]) bucket.low_y = i;
            if (y[i] > y[bucket.high_y]) bucket.high_y = i;
        }
    }
    flush_bucket();
    flush_line();
    canvas.Restore(saved);
}
void draw_chart(Graphics& canvas, float width, float height, const Plot& plot,
                const std::wstring& filename)
{
    canvas.Clear(paper);
    canvas.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    const Font title(L"Microsoft YaHei UI", 19.0f, FontStyleBold, UnitPixel);
    const Font axis(L"Microsoft YaHei UI", 13.0f, FontStyleRegular, UnitPixel);
    const Font tick(L"Segoe UI", 12.5f, FontStyleRegular, UnitPixel);
    const Pen frame(grid);
    canvas.DrawRectangle(&frame, 0.5f, 0.5f, width - 1, height - 1);
    const RectF area = plot_area(width, height);
    const float bottom = area.GetBottom();
    text(canvas, filename, title, ink, RectF(18.0f, 13.0f, width - 36.0f, 26.0f));
    text(canvas, plot.y_style.label, axis, ink, RectF(area.X, 44.0f, area.Width, 20.0f));
    text(canvas, plot.x_style.label, axis, ink, RectF(area.X, bottom + 32.0f, area.Width, 24.0f),
         StringAlignmentCenter);
    const SolidBrush plot_background(Color(250, 252, 254));
    canvas.FillRectangle(&plot_background, area);
    const auto x_divisions = std::clamp(static_cast<int>(area.Width / 98.0f), 2, 12);
    const auto y_divisions = std::clamp(static_cast<int>(area.Height / 44.0f), 2, 12);
    ticks(plot.view.x, plot.x_style, x_divisions, [&](float position, const std::wstring& label) {
        const float x = area.X + position * area.Width;
        canvas.DrawLine(&frame, x, area.Y, x, bottom);
        text(canvas, label, tick, muted, RectF(x - 48.0f, bottom + 6.0f, 96.0f, 20.0f),
             StringAlignmentCenter);
    });
    ticks(plot.view.y, plot.y_style, y_divisions, [&](float position, const std::wstring& label) {
        const float y = bottom - position * area.Height;
        canvas.DrawLine(&frame, area.X, y, area.GetRight(), y);
        text(canvas, label, tick, muted, RectF(4.0f, y - 11.0f, area.X - 12.0f, 22.0f),
             StringAlignmentFar);
    });
    canvas.DrawRectangle(&frame, area);
    const Pen axes(Color(100, 126, 146), 1.1f);
    canvas.DrawLine(&axes, area.X, area.Y, area.X, bottom);
    canvas.DrawLine(&axes, area.X, bottom, area.GetRight(), bottom);
    if (plot.point_count > 0) {
        draw_curve(canvas, area, plot);
    } else {
        text(canvas, L"所选列没有可绘制的第一象限数据", axis, muted, area, StringAlignmentCenter);
    }
}
} // namespace

Gdiplus::RectF plot_area(float width, float height)
{
    return {116.0f, 68.0f, (std::max)(1.0f, width - 144.0f), (std::max)(1.0f, height - 133.0f)};
}

std::wstring coordinate_label(const Range& range, const AxisStyle& style, double fraction,
                              double pixels)
{
    const double value = (range.low + range.span() * std::clamp(fraction, 0.0, 1.0)) * style.multiplier;
    if (!std::isfinite(value)) {
        return L"超出范围";
    }
    // Reuse the axis number format, with finer precision for the mouse position.
    const double step = (std::max)(range.span() * style.multiplier / (std::max)(pixels, 1.0),
                                   std::numeric_limits<double>::min());
    std::wstring text = tick_label(value, step, style.decimals);
    if (style.unit == L"%") {
        text += L"%";
    } else if (!style.unit.empty()) {
        text += L" " + style.unit;
    }
    return text;
}

std::shared_ptr<Gdiplus::Bitmap> render_chart(int width, int height, float scale, const Plot& plot,
                                              const std::wstring& filename)
{
    auto bitmap = std::shared_ptr<Bitmap>(new Bitmap(width, height, PixelFormat24bppRGB));
    if (bitmap->GetLastStatus() != Ok) {
        throw std::runtime_error("无法分配曲线图像。");
    }
    Graphics canvas(bitmap.get());
    canvas.ScaleTransform(scale, scale);
    draw_chart(canvas, static_cast<float>(width) / scale, static_cast<float>(height) / scale,
               plot, filename);
    return bitmap;
}

bool save_jpeg(const std::filesystem::path& path, Gdiplus::Bitmap& bitmap)
{
    CLSID encoder = {};
    if (FAILED(CLSIDFromString(L"{557CF401-1A04-11D3-9A73-0000F81EF32E}", &encoder))) {
        return false;
    }
    ULONG quality = 94;
    EncoderParameters parameters = {};
    parameters.Count = 1;
    parameters.Parameter[0] = {EncoderQuality, 1, EncoderParameterValueTypeLong, &quality};
    return bitmap.Save(path.c_str(), &encoder, &parameters) == Ok;
}
