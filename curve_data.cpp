#include "curve_data.h"
#include "csv.h"

#include <windows.h>

#include <array>
#include <cmath>
#include <cstdint>
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
class BufferedInput {
public:
    explicit BufferedInput(std::istream& input) : input_(input) {}

    const char* current() const { return next_; }
    const char* end() const { return end_; }
    void advance(const char* next) { next_ = next; }

    int sgetc()
    {
        if (next_ == end_) {
            input_.read(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
            if (input_.bad()) {
                throw std::runtime_error("读取 CSV 文件失败。");
            }
            next_ = buffer_.data();
            end_ = next_ + input_.gcount();
            if (next_ == end_) return std::char_traits<char>::eof();
        }
        return std::char_traits<char>::to_int_type(*next_);
    }

    int sbumpc()
    {
        const auto value = sgetc();
        if (value != std::char_traits<char>::eof()) ++next_;
        return value;
    }

private:
    std::istream& input_;
    std::array<char, 256 * 1024> buffer_;
    const char* next_ = buffer_.data();
    const char* end_ = next_;
};

class CsvRecords {
public:
    explicit CsvRecords(std::istream& input) : input_(input) {}

    bool read(std::vector<std::string_view>& fields)
    {
        using Traits = std::char_traits<char>;
        while (input_.sgetc() != Traits::eof()) {
            if (skip_lf_) {
                skip_lf_ = false;
                if (input_.sgetc() == '\n') input_.sbumpc();
            }
            fields.clear();
            // Keep unquoted fields as views into the buffer. The general CSV
            // parser handles quoted records and records that cross a refill.
            const auto simple = read_unquoted(fields);
            if (simple == Record::fields) return true;
            if (simple == Record::blank) continue;

            if (!csv::read_record(input_, owned_fields_)) return false;
            fields.assign(owned_fields_.begin(), owned_fields_.end());
            return true;
        }
        return false;
    }

private:
    enum class Record { fields, blank, fallback };

    Record read_unquoted(std::vector<std::string_view>& fields)
    {
        const char* start = input_.current();
        bool blank = true;
        for (const char* next = start; next != input_.end(); ++next) {
            const char ch = *next;
            if (ch == ',') {
                fields.emplace_back(start, static_cast<std::size_t>(next - start));
                start = next + 1;
                blank = false;
            } else if (ch == '\n' || ch == '\r') {
                input_.advance(next + 1);
                // Delay the LF check so the caller's views survive a refill.
                skip_lf_ = ch == '\r';
                if (blank) return Record::blank;
                fields.emplace_back(start, static_cast<std::size_t>(next - start));
                return Record::fields;
            } else if (ch == '"') {
                break;
            } else if (blank && ch != ' ' && ch != '\t') {
                blank = false;
            }
        }
        return Record::fallback;
    }

    BufferedInput input_;
    std::vector<std::string> owned_fields_;
    bool skip_lf_ = false;
};

bool plain_decimal(std::string_view text, double& result)
{
    if (text.empty() || text.size() > 17) return false;
    const bool negative = text.front() == '-';
    if (negative || text.front() == '+') text.remove_prefix(1);
    std::uint64_t digits = 0;
    int count = 0;
    int fraction = -1;
    for (const char ch : text) {
        if (ch >= '0' && ch <= '9') {
            if (++count > 15) return false;
            digits = digits * 10 + static_cast<unsigned>(ch - '0');
            if (fraction >= 0) ++fraction;
        } else if (ch == '.' && fraction < 0) {
            fraction = 0;
        } else {
            return false;
        }
    }
    if (count == 0) return false;
    // Both operands are exact doubles for up to 15 decimal digits.
    constexpr double powers[] = {1.0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7,
                                 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15};
    const double value = static_cast<double>(digits);
    const double magnitude = fraction > 0 ? value / powers[fraction] : value;
    result = negative ? -magnitude : magnitude;
    return true;
}

double plot_value(std::string_view field, bool& percent)
{
    constexpr double missing = std::numeric_limits<double>::quiet_NaN();
    if (field.empty()) {
        return missing;
    }
    const auto last = field.find_last_not_of(" \t\r\n");
    const bool has_percent = last != std::string::npos && field[last] == '%';
    const auto text = has_percent ? field.substr(0, last) : field;
    double value = 0.0;
    if ((plain_decimal(text, value) || csv::parse_number(std::string(text), value)) && std::isfinite(value)) {
        percent = percent || has_percent;
        return value;
    }
    const auto first = field.find_first_not_of(" \t\r\n");
    if (has_percent || first == std::string::npos) {
        return missing;
    }
    const auto token = field.substr(first, last - first + 1);
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
    CsvRecords records(input);
    std::vector<std::string_view> fields;
    if (!records.read(fields)) {
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

    while (records.read(fields)) {
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
