#pragma once

#include "curve_data.h"

#include <memory>
#include <windows.h>
#include <gdiplus.h>

struct Range {
    double low = 0.0;
    double high = 1.0;

    double span() const { return high - low; }
};

struct View {
    Range x;
    Range y;
};

struct AxisStyle {
    std::wstring label;
    double multiplier = 1.0;
    int decimals = -1;
    std::wstring unit;
};

struct Plot {
    const Column* x = nullptr;
    const Column* y = nullptr;
    AxisStyle x_style;
    AxisStyle y_style;
    View view;
    std::size_t point_count = 0;
};

Gdiplus::RectF plot_area(float width, float height);
std::wstring coordinate_label(const Range& range, const AxisStyle& style, double fraction,
                              double pixels);
std::shared_ptr<Gdiplus::Bitmap> render_chart(int width, int height, float scale, const Plot& plot,
                                              const std::wstring& filename);
bool save_jpeg(const std::filesystem::path& path, Gdiplus::Bitmap& bitmap);
