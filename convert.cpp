#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
namespace fs = std::filesystem;

void print(const std::string& message)
{
#ifdef _WIN32
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(output, &mode)) {
        const std::wstring wide = fs::u8path(message).wstring();
        DWORD written = 0;
        if (WriteConsoleW(output, wide.data(), static_cast<DWORD>(wide.size()),
                          &written, nullptr)) {
            return;
        }
    }
#endif
    std::cout << message << std::flush;
}

std::string read_filename()
{
    print("请输入CSV文件名（例如：decoder.csv）：");
    std::string filename;
#ifdef _WIN32
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(input, &mode)) {
        wchar_t buffer[32768];
        DWORD length = 0;
        if (ReadConsoleW(input, buffer, 32768, &length, nullptr)) {
            filename = fs::path(std::wstring(buffer, length)).u8string();
        }
    } else
#endif
    {
        std::getline(std::cin, filename);
    }
    const auto start = filename.find_first_not_of(" \t\r\n\v\f");
    if (start == std::string::npos) {
        return {};
    }
    return filename.substr(start, filename.find_last_not_of(" \t\r\n\v\f") - start + 1);
}

// Read one CSV record, preserving commas and newlines inside quoted fields.
bool read_record(std::streambuf& input, std::vector<std::string>& fields)
{
    using Traits = std::char_traits<char>;
    std::size_t column = 0;
    bool quoted = false;
    bool at_field_start = true;
    bool blank = true;
    if (fields.empty()) {
        fields.emplace_back();
    }
    fields.front().clear();

    for (int value; (value = input.sbumpc()) != Traits::eof();) {
        const char ch = Traits::to_char_type(value);
        if (quoted) {
            if (ch != '"') {
                fields[column].push_back(ch);
            } else if (input.sgetc() == '"') {
                input.sbumpc();
                fields[column].push_back('"');
            } else {
                quoted = false;
            }
        } else if (ch == '\r' || ch == '\n') {
            if (ch == '\r' && input.sgetc() == '\n') {
                input.sbumpc();
            }
            if (!blank) {
                break;
            }
            fields.front().clear();
            at_field_start = true;
        } else if (ch == ',') {
            ++column;
            if (column == fields.size()) {
                fields.emplace_back();
            } else {
                fields[column].clear();
            }
            at_field_start = true;
            blank = false;
        } else {
            if (ch == '"' && at_field_start) {
                quoted = true;
            } else {
                fields[column].push_back(ch);
            }
            at_field_start = false;
            blank = blank && (ch == ' ' || ch == '\t');
        }
    }
    if (quoted) {
        throw std::runtime_error("CSV 中的引号未闭合。");
    }
    fields.resize(column + 1);
    return !blank;
}

bool is_missing(std::string_view field)
{
    constexpr std::string_view markers[] = {
        "", "#N/A", "#N/A N/A", "#NA", "-1.#IND", "-1.#QNAN", "-NaN", "-nan",
        "1.#IND", "1.#QNAN", "<NA>", "N/A", "NA", "NULL", "NaN", "None",
        "n/a", "nan", "null",
    };
    return std::find(std::begin(markers), std::end(markers), field) != std::end(markers);
}

bool parse_number(const std::string& field, double& number)
{
    const char* start = field.c_str();
    char* end = nullptr;
    number = std::strtod(start, &end);
    if (end == start) {
        return false;
    }
    end += std::strspn(end, " \t\r\n\v\f");
    return end == start + field.size();
}

double number_from(const std::string& field, const char* column, std::size_t row)
{
    double number = 0.0;
    if (!parse_number(field, number)) {
        throw std::runtime_error("第 " + std::to_string(row) +
                                 " 条数据的 " + column + " 无法转成数字。");
    }
    return number;
}

struct DutyAverage {
    void add(double value)
    {
        if (std::isnan(value)) {
            return;
        }
        // Compensated summation matches the precision of pandas groupby.mean.
        const double corrected = value - compensation;
        const double next_sum = sum + corrected;
        compensation = (next_sum - sum) - corrected;
        if (std::isnan(compensation)) {
            compensation = 0.0;
        }
        sum = next_sum;
        ++count;
    }

    double mean() const
    {
        return count == 0 ? std::numeric_limits<double>::quiet_NaN()
                          : sum / static_cast<double>(count);
    }

    double sum = 0.0;
    double compensation = 0.0;
    std::size_t count = 0;
};

std::string format_number(double value)
{
    const double magnitude = std::abs(value);
    const bool scientific = std::isfinite(value) && value != 0.0 &&
                            (magnitude < 1e-4 || magnitude >= 1e16);
    char buffer[64];
    const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value,
                                      scientific ? std::chars_format::scientific
                                                 : std::chars_format::fixed);
    std::string text(buffer, result.ptr);
    if (std::isfinite(value) && !scientific && text.find('.') == std::string::npos) {
        text += ".0";
    }
    return text;
}

std::string output_filename_for(std::string filename)
{
    for (std::size_t position = 0;
         (position = filename.find(".csv", position)) != std::string::npos;
         position += 7) {
        filename.replace(position, 4, "_v0.csv");
    }
    return filename;
}

int convert(const std::string& filename)
{
    std::ifstream input(fs::u8path(filename), std::ios::binary);
    if (!input) {
        if (fs::exists(fs::u8path(filename))) {
            throw std::runtime_error("无法读取文件 '" + filename + "'。");
        }
        print("❌ 文件 '" + filename + "' 未找到，请检查路径和文件名。\n");
        return 1;
    }

    // pandas accepts a UTF-8 BOM before the header.
    char prefix[3];
    if (!input.read(prefix, sizeof(prefix)) ||
        std::string_view(prefix, sizeof(prefix)) != "\xef\xbb\xbf") {
        input.clear();
        input.seekg(0);
    }
    std::vector<std::string> fields;
    if (!read_record(*input.rdbuf(), fields)) {
        throw std::runtime_error("CSV 文件中没有表头。");
    }
    const auto time = std::find(fields.begin(), fields.end(), "Time[ns]");
    const auto duty = std::find(fields.begin(), fields.end(), "PWM: Duty cycle");
    if (time == fields.end() || duty == fields.end()) {
        throw std::runtime_error("CSV 必须包含 Time[ns] 和 PWM: Duty cycle 列。");
    }
    const auto time_column = static_cast<std::size_t>(time - fields.begin());
    const auto duty_column = static_cast<std::size_t>(duty - fields.begin());
    const std::size_t header_width = fields.size();

    std::map<double, DutyAverage> groups;
    std::size_t rows = 0;
    std::size_t index_columns = 0;
    bool duty_is_text = false;
    while (read_record(*input.rdbuf(), fields)) {
        ++rows;
        if (rows == 1 && fields.size() > header_width) {
            index_columns = fields.size() - header_width;
        }
        if (fields.size() > header_width + index_columns) {
            throw std::runtime_error("第 " + std::to_string(rows) + " 条数据的字段过多。");
        }
        fields.resize(header_width + index_columns);
        const auto& timestamp = fields[time_column + index_columns];
        auto& duty_text = fields[duty_column + index_columns];

        double seconds = std::numeric_limits<double>::quiet_NaN();
        if (!is_missing(timestamp)) {
            seconds = number_from(timestamp, "Time[ns]", rows) / 1e9;
            seconds = std::nearbyint(seconds * 10.0) / 10.0;
        }

        double duty_value = std::numeric_limits<double>::quiet_NaN();
        if (!is_missing(duty_text)) {
            duty_is_text = duty_is_text || !parse_number(duty_text, duty_value);
            duty_text.erase(std::remove(duty_text.begin(), duty_text.end(), '%'), duty_text.end());
            duty_value = number_from(duty_text, "PWM: Duty cycle", rows);
        }
        if (!std::isnan(seconds)) {
            groups[seconds].add(duty_value);
        }
    }
    if (rows != 0 && !duty_is_text) {
        throw std::runtime_error("PWM: Duty cycle 列须包含百分比文本。");
    }
    input.close();

    const std::string output_filename = output_filename_for(filename);
    std::ofstream output(fs::u8path(output_filename));
    if (!output) {
        throw std::runtime_error("无法创建输出文件 '" + output_filename + "'。");
    }
    output << "Id,Time_s,Duty_%\n";
    std::size_t id = 0;
    for (const auto& [seconds, average] : groups) {
        const double duty_value = std::nearbyint(average.mean() * 100.0) / 100.0;
        output << ++id << ',' << format_number(seconds) << ',';
        if (!std::isnan(duty_value)) {
            output << format_number(duty_value) << '%';
        }
        output << '\n';
    }
    output.close();
    if (!output) {
        throw std::runtime_error("写入输出文件失败 '" + output_filename + "'。");
    }

    print("✅ 转换完成！输出文件：" + output_filename + "\n");
    print("原数据行数: " + std::to_string(rows) + " 行\n");
    // Keep the original message. The calculation above uses one decimal place.
    print("按0.01秒聚合后行数: " + std::to_string(groups.size()) + " 行\n");
    return 0;
}

int run(std::string filename, bool has_argument)
{
    if (!has_argument) {
        filename = read_filename();
        if (filename.empty()) {
            print("❌ 未输入文件名，程序退出。\n");
            return 1;
        }
    }
    try {
        return convert(filename);
    } catch (const std::exception& error) {
        print(std::string("❌ ") + error.what() + "\n");
        return 1;
    }
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[])
{
    return run(argc > 1 ? fs::path(argv[1]).u8string() : std::string{}, argc > 1);
}
#else
int main(int argc, char* argv[])
{
    return run(argc > 1 ? argv[1] : "", argc > 1);
}
#endif
