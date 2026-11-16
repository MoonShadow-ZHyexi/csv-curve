#pragma once

#include "curve_data.h"

#include <optional>

struct PwmOptions {
    double seconds_per_unit = 1.0;
    int time_decimals = 1;
    bool active_high = true;
    std::optional<double> threshold = std::nullopt;

    bool operator==(const PwmOptions& other) const
    {
        return seconds_per_unit == other.seconds_per_unit && time_decimals == other.time_decimals &&
               active_high == other.active_high && threshold == other.threshold;
    }
};

struct PwmSeries {
    Column time{L"时间 [s]", {}, true};
    Column duty{L"占空比 [%]", {}, false};
    std::size_t periods = 0;
    std::size_t repaired = 0;
};

bool duty_column(const Column& column);
PwmSeries pwm_series(const Column& time, const Column& signal, const PwmOptions& options,
                     const std::atomic_bool& cancel);
void save_pwm_csv(const std::filesystem::path& path, const PwmSeries& series);
