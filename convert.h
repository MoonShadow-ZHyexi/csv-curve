#pragma once

#include <atomic>
#include <cstddef>
#include <string>

struct PwmConversion {
    std::string output_filename;
    std::size_t rows = 0;
    std::size_t groups = 0;
};

std::string pwm_output_filename(std::string filename);
PwmConversion convert_pwm(const std::string& filename, int time_decimals = 1,
                          const std::atomic_bool* cancel = nullptr);
