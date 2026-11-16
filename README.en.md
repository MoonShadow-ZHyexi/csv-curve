# CSV Curve

[简体中文](./README.md)

CSV Curve is a Windows desktop tool for plotting CSV files and converting PWM signals to duty-cycle data. It is written in C++ and runs as a standalone executable.

## Download and run

Download the executable for your Windows system from [GitHub Releases](https://github.com/MoonShadow-ZHyexi/csv-curve/releases/latest).

| Windows architecture | Download |
| --- | --- |
| 32-bit (x86) | [csv_curve_x86.exe](https://github.com/MoonShadow-ZHyexi/csv-curve/releases/latest/download/csv_curve_x86.exe) |
| 64-bit (x64) | [csv_curve.exe](https://github.com/MoonShadow-ZHyexi/csv-curve/releases/latest/download/csv_curve.exe) |

Double-click the downloaded file to run it. Both builds target Windows 7 and include the application icon. If an older copy reports a missing `GetDpiForSystem` entry point, replace it with the current release.

## Open a CSV and plot it

1. Drag the file into the window or click **Open CSV** (打开 CSV).
2. Select a data mode and the columns you want to use.
3. Click **Generate Chart** (生成曲线) to draw the first chart. After you change an option, the chart updates automatically about 0.3 seconds after you stop making changes.

Use comma-separated CSV with column names in the first row. The plot displays points with nonnegative X and Y values. The interface labels shown in parentheses match the application.

### Regular data

Choose **Regular Data** (常规数据), then select the X and Y columns. Set the unit conversion and tick precision for each axis as needed. For a time column in seconds, choose **Seconds to Minutes** (秒 → 分钟) or **Seconds to Hours** (秒 → 小时) to change the displayed X unit.

```csv
Time_s,Voltage[V]
0,1.2
30,2.1
60,3.3
```

### PWM data

Choose **Raw PWM Data** (原始 PWM 方波数据), then select the time and signal columns. The tool accepts digital levels, sampled voltage values, and precomputed duty-cycle readings.

- **Digital levels or sampled voltages.** Choose **Active High** (高有效) or **Active Low** (低有效). The tool calculates duty cycle from the time the signal stays at the selected active level during each complete period. Leave **Level Threshold** (电平阈值) blank for automatic detection, or enter a numeric threshold.
- **Precomputed duty cycle.** For files with columns such as `Time[ns]` and `PWM: Duty cycle`, Active Low converts each high-state duty cycle to `100% − high-state duty cycle`.
- **Time and grouping.** Set **Input Time Unit** (输入时间单位) to match the CSV. **Display Unit** (显示单位) controls whether the X axis shows seconds, minutes, or hours. **PWM Grouping** (PWM 分组) defaults to 0.1 seconds. The tool averages the duty-cycle readings within each group.

This digital signal uses microseconds and has a 30% active-high duty cycle.

```csv
Time[us],PWM
0,0
10,1
40,0
110,1
140,0
210,1
240,0
310,1
```

## Navigate and export

| Task | How to do it |
| --- | --- |
| Zoom | Scroll the mouse wheel over the plot or click **+** or **−**. |
| Pan | Drag inside the plot with the left mouse button. The view updates when you release it. |
| Reset the view | Click **Full View** (全图). |
| Read coordinates | Move the pointer over the plot to see its X and Y coordinates and any labeled units next to the crosshair. |
| Save a JPG | Click **Save JPG** (保存 JPG) to export the current view. |
| Export converted PWM data | Generate a PWM chart, then click **Save Converted CSV** (保存转换 CSV). The output columns are `Id`, `Time_s`, and `Duty_%`. The time values are in seconds. |

## Build from source

Use CMake 3.21 or later and the Visual Studio 2022 MSVC v143 C++ toolset. To build the 32-bit Windows 7 edition, run the following commands from the repository root.

```powershell
cmake -S . -B build-x86 -G "Visual Studio 17 2022" -A Win32 -DCSV_CURVE_WIN7=ON
cmake --build build-x86 --config Release --target csv_curve
```

The executable is written to `build-x86/Release/csv_curve.exe`. For a 64-bit build, use `-A x64` and a separate build directory.

## License

[MIT License](./LICENSE)
