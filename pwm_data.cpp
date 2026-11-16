#include "pwm_data.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <limits>
#include <stdexcept>

bool duty_column(const Column& column)
{
    std::wstring name = column.name;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return column.percent || name.find(L"duty") != std::wstring::npos ||
           name.find(L"占空比") != std::wstring::npos;
}

namespace {
bool cancelled(const std::atomic_bool& cancel, std::size_t row)
{
    return row % 4096 == 0 && cancel.load(std::memory_order_relaxed);
}

bool valid_duty(double duty)
{
    return duty >= 0.0 && duty <= 100.0;
}

class Groups {
public:
    explicit Groups(const PwmOptions& options)
        : units_per_second_(1.0 / options.seconds_per_unit),
          scale_(std::pow(10.0, options.time_decimals)) {}

    void add(double raw_time, double active_duty)
    {
        const double time = std::nearbyint(raw_time / units_per_second_ * scale_) / scale_;
        if (!std::isfinite(time)) {
            return;
        }
        if (count_ && time != current_) {
            flush();
        }
        current_ = time;
        sum_ += active_duty;
        ++count_;
        ++series_.periods;
    }

    void repaired() { ++series_.repaired; }

    PwmSeries finish()
    {
        if (count_) {
            flush();
        }
        if (series_.periods == 0) {
            throw std::runtime_error("没有读到有效占空比或完整的 PWM 周期。请检查时间列和信号列。");
        }
        return std::move(series_);
    }

private:
    void flush()
    {
        series_.time.values.push_back(current_);
        series_.duty.values.push_back(std::nearbyint(sum_ * 100.0 / static_cast<double>(count_)) / 100.0);
        sum_ = 0.0;
        count_ = 0;
    }

    PwmSeries series_;
    double units_per_second_;
    double scale_;
    double current_ = 0.0;
    double sum_ = 0.0;
    std::size_t count_ = 0;
};

// Only join a high/low pair when its combined duration agrees with the adjacent
// periods and its reconstructed duty agrees with the adjacent measurements.
bool split_period(const std::vector<double>& t, const std::vector<double>& y,
                  std::size_t i, double& merged)
{
    if (i == 0 || i + 3 >= t.size() || y[i] < 99.0 || y[i + 1] > 1.0 ||
        !valid_duty(y[i + 1]) || !valid_duty(y[i - 1]) || !valid_duty(y[i + 2])) {
        return false;
    }
    const double first = t[i + 1] - t[i];
    const double second = t[i + 2] - t[i + 1];
    const double period = first + second;
    const double before = t[i] - t[i - 1];
    const double after = t[i + 3] - t[i + 2];
    if (!(first > 0.0 && second > 0.0 && before > 0.0 && after > 0.0) ||
        std::abs(period - before) > before * 0.05 ||
        std::abs(period - after) > after * 0.05) {
        return false;
    }
    merged = (first / period) * y[i] + (second / period) * y[i + 1];
    return std::abs(merged - y[i - 1]) <= 2.0 && std::abs(merged - y[i + 2]) <= 2.0;
}

void add_decoded(const Column& time, const Column& duty, const PwmOptions& options,
                 const std::atomic_bool& cancel, Groups& groups)
{
    const auto& t = time.values;
    const auto& y = duty.values;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (cancelled(cancel, i)) return;
        double value = y[i];
        if (!std::isfinite(t[i]) || !valid_duty(value)) {
            continue;
        }
        const bool repaired = split_period(t, y, i, value);
        groups.add(t[i], options.active_high ? value : 100.0 - value);
        if (repaired) {
            ++i;
            groups.repaired();
        }
    }
}

void add_samples(const Column& time, const Column& signal, const PwmOptions& options,
                 const std::atomic_bool& cancel, Groups& groups)
{
    const auto& t = time.values;
    const auto& level = signal.values;
    double threshold = options.threshold.value_or(0.0);
    if (!options.threshold) {
        double low = std::numeric_limits<double>::infinity();
        double high = -low;
        for (std::size_t i = 0; i < level.size(); ++i) {
            if (cancelled(cancel, i)) return;
            if (std::isfinite(level[i]) && std::isfinite(t[i])) {
                low = std::min(low, level[i]);
                high = std::max(high, level[i]);
            }
        }
        if (!(low < high)) {
            throw std::runtime_error("方波信号列中没有检测到高低电平的变化。");
        }
        threshold = std::max(low / 2.0 + high / 2.0, std::nextafter(low, high));
    }
    constexpr double unset = std::numeric_limits<double>::quiet_NaN();
    double cycle_start = unset;
    double inactive_start = unset;
    int previous = -1;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (cancelled(cancel, i)) return;
        const double value = level[i];
        if (!std::isfinite(value) || !std::isfinite(t[i])) {
            previous = -1;
            cycle_start = inactive_start = unset;
            continue;
        }
        const int active = (value >= threshold) == options.active_high ? 1 : 0;
        if (previous >= 0 && active != previous) {
            if (active) {
                const double period = t[i] - cycle_start;
                if (period > 0.0 && std::isfinite(period) && std::isfinite(inactive_start)) {
                    const double ratio = std::clamp((inactive_start - cycle_start) / period, 0.0, 1.0);
                    groups.add(cycle_start, 100.0 * ratio);
                }
                cycle_start = t[i];
                inactive_start = unset;
            } else {
                inactive_start = t[i];
            }
        }
        previous = active;
    }
}

std::string csv_number(double value, int decimals)
{
    char buffer[340];
    const auto formatted = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                         std::chars_format::fixed, decimals);
    std::string result(buffer, formatted.ptr);
    const auto point = result.find('.');
    if (point != std::string::npos) {
        while (result.size() > point + 2 && result.back() == '0') {
            result.pop_back();
        }
    }
    return result;
}
} // namespace

PwmSeries pwm_series(const Column& time, const Column& signal, const PwmOptions& options,
                     const std::atomic_bool& cancel)
{
    if (time.values.size() != signal.values.size() || &time == &signal ||
        options.seconds_per_unit <= 0.0 || !std::isfinite(options.seconds_per_unit) ||
        options.time_decimals < 0 || options.time_decimals > 9 ||
        (options.threshold && !std::isfinite(*options.threshold))) {
        throw std::runtime_error("请分别选择有效的时间列和 PWM 信号列，并检查时间单位和分组精度。");
    }
    if (!time.ascending) {
        double last = -std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < time.values.size(); ++i) {
            if (cancelled(cancel, i)) return {};
            const double value = time.values[i];
            if (std::isfinite(value)) {
                if (value < last) {
                    throw std::runtime_error("PWM 模式要求时间按升序排列。");
                }
                last = value;
            }
        }
    }
    Groups groups(options);
    if (duty_column(signal)) {
        add_decoded(time, signal, options, cancel, groups);
    } else {
        add_samples(time, signal, options, cancel, groups);
    }
    return cancel.load(std::memory_order_relaxed) ? PwmSeries{} : groups.finish();
}

void save_pwm_csv(const std::filesystem::path& path, const PwmSeries& series)
{
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("无法创建转换后的 CSV。");
    }
    output << "Id,Time_s,Duty_%\n";
    for (std::size_t i = 0; i < series.time.values.size(); ++i) {
        output << i + 1 << ',' << csv_number(series.time.values[i], 9) << ','
               << csv_number(series.duty.values[i], 2) << "%\n";
    }
    output.close();
    if (!output) {
        throw std::runtime_error("写入转换后的 CSV 失败。");
    }
}
