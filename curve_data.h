#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct Column {
    std::wstring name;
    std::vector<double> values;
    bool ascending = true;
    bool percent = false;
};

struct CsvTable {
    std::filesystem::path path;
    std::vector<Column> columns;
    std::size_t rows = 0;
};

std::wstring wide_text(std::string_view text);
CsvTable load_table(const std::filesystem::path& path, const std::atomic_bool& cancel,
                    std::atomic_size_t& progress);
