#pragma once

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace csv {
// Read one CSV record, preserving commas and newlines inside quoted fields.
inline bool read_record(std::streambuf& input, std::vector<std::string>& fields)
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

inline bool is_missing(std::string_view field)
{
    constexpr std::string_view markers[] = {
        "", "#N/A", "#N/A N/A", "#NA", "-1.#IND", "-1.#QNAN", "-NaN", "-nan",
        "1.#IND", "1.#QNAN", "<NA>", "N/A", "NA", "NULL", "NaN", "None",
        "n/a", "nan", "null",
    };
    return std::find(std::begin(markers), std::end(markers), field) != std::end(markers);
}

inline bool parse_number(const std::string& field, double& number)
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

inline void skip_bom(std::ifstream& input)
{
    // pandas accepts a UTF-8 BOM before the header.
    char prefix[3];
    if (!input.read(prefix, sizeof(prefix)) ||
        std::string_view(prefix, sizeof(prefix)) != "\xef\xbb\xbf") {
        input.clear();
        input.seekg(0);
    }
}
} // namespace csv
