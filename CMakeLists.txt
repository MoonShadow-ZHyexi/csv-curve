cmake_minimum_required(VERSION 3.21)
project(CsvCurves LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8 /W4 /EHsc /fp:precise)
endif()

add_executable(convert convert.cpp)

if(WIN32)
    add_executable(csv_curve WIN32 curve_app.cpp curve_chart.cpp curve_data.cpp pwm_data.cpp)
    target_compile_definitions(csv_curve PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(csv_curve PRIVATE gdiplus comdlg32 shell32 ole32 gdi32 user32)
    if(MINGW)
        target_link_options(csv_curve PRIVATE -municode)
        target_link_options(convert PRIVATE -municode)
    endif()
endif()
