#include "curve_data.h"
#include "csv.h"

#include <windows.h>

#include <cmath>
#include <limits>
#include <stdexcept>

std::wstring wide_text(std::string_view text)
{
    if (text.empty()) {
        return {};
    }
    const int length = static_cast<int>(text.size());
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int count = MultiByteToWideChar(code_page, flags, text.data(), length, nullptr, 0);
    if (count == 0) {
        code_page = GetACP();
        flags = 0;
        count = MultiByteToWideChar(code_page, flags, text.data(), length, nullptr, 0);
    }
    std::wstring result(count, L'\0');
    if (count > 0) {
        MultiByteToWideChar(code_page, flags, text.data(), length, result.data(), count);
    }
    return result;
}

namespace {
double plot_value(std::string& field, bool& percent)
{
    constexpr double missing = std::numeric_limits<double>::quiet_NaN();
    if (field.empty()) {
        return missing;
    }
    const auto last = field.find_last_not_of(" \t\r\n");
    const bool has_percent = last != std::string::npos && field[last] == '%';
    if (has_percent) {
        field.resize(last);
    }
    double value = 0.0;
    if (csv::parse_number(field, value) && std::isfinite(value)) {
        percent = percent || has_percent;
        return value;
    }
    const auto first = field.find_first_not_of(" \t\r\n");
    if (has_percent || first == std::string::npos) {
        return missing;
    }
    const std::string_view token(field.data() + first, last - first + 1);
    if (token == "HIGH" || token == "High" || token == "high") return 1.0;
    if (token == "LOW" || token == "Low" || token == "low") return 0.0;
    return missing;
}
} // namespace

CsvTable load_table(const std::filesystem::path& path, const std::atomic_bool& cancel,
                    std::atomic_size_t& progress)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("无法打开 CSV 文件。");
    }
    csv::skip_bom(input);
    std::vector<std::string> fields;
    if (!csv::read_record(*input.rdbuf(), fields)) {
        throw std::runtime_error("CSV 为空，第一行应为列名。");
    }

    CsvTable table;
    table.path = path;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        auto name = wide_text(fields[index]);
        if (name.empty()) {
            name = L"第 " + std::to_wstring(index + 1) + L" 列";
        }
        // Always display the column number so duplicate headers remain selectable.
        table.columns.push_back({std::move(name), {}, true});
    }

    while (csv::read_record(*input.rdbuf(), fields)) {
        if (table.rows % 4096 == 0) {
            if (cancel.load(std::memory_order_relaxed)) {
                return {};
            }
            progress.store(table.rows, std::memory_order_relaxed);
        }
        if (fields.size() > table.columns.size()) {
            throw std::runtime_error("第 " + std::to_string(table.rows + 2) +
                                     " 条 CSV 记录的字段数超过表头列数。");
        }
        fields.resize(table.columns.size());
        for (std::size_t index = 0; index < fields.size(); ++index) {
            auto& column = table.columns[index];
            const double value = plot_value(fields[index], column.percent);
            if (column.values.empty()) {
                if (std::isnan(value)) {
                    continue;
                }
                if (table.rows > 0) {
                    column.values.resize(table.rows, std::numeric_limits<double>::quiet_NaN());
                    column.ascending = false;
                }
            } else if (column.ascending && !(value >= column.values.back())) {
                column.ascending = false;
            }
            column.values.push_back(value);
        }
        ++table.rows;
    }
    if (cancel.load(std::memory_order_relaxed)) {
        return {};
    }
    if (table.rows == 0) {
        throw std::runtime_error("CSV 只有表头，未找到数据。");
    }
    if (std::none_of(table.columns.begin(), table.columns.end(),
                     [](const Column& column) { return !column.values.empty(); })) {
        throw std::runtime_error("CSV 中未找到可绘制的数值列。");
    }
    return table;
}
