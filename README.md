# CSV 数据曲线

[English](./README.en.md)

Windows 上的 CSV 绘图与 PWM 占空比转换工具，使用 C++ 编写。支持拖入 CSV、选择坐标列、缩放和平移、十字线读数，以及导出 JPG 和转换后的 CSV。

## 下载和运行

从 [GitHub Releases](https://github.com/MoonShadow-ZHyexi/csv-curve/releases/latest) 下载。根据 Windows 的系统位数选择版本。

| Windows 系统 | 下载 |
| --- | --- |
| 32 位（x86） | [csv_curve_x86.exe](https://github.com/MoonShadow-ZHyexi/csv-curve/releases/latest/download/csv_curve_x86.exe) |
| 64 位（x64） | [csv_curve.exe](https://github.com/MoonShadow-ZHyexi/csv-curve/releases/latest/download/csv_curve.exe) |

双击 EXE 即可运行，无需安装 Python。两个版本均面向 Windows 7 构建，包含程序图标。若旧版在 Windows 7 上报 `GetDpiForSystem` 入口错误，请用这里的新版 EXE 替换。

## 导入和绘图

1. 将 CSV 拖入窗口，或点击「打开 CSV」。
2. 选择数据模式和对应的数据列。
3. 点击「生成曲线」即可查看结果。如果调整了选项，程序会在停止调整约 0.3 秒后自动重新生成。

CSV 使用逗号分隔，第一行是列名。绘图区显示 X、Y 均大于或等于 0 的数据。

### 常规数据

选择「常规数据」，分别指定 X 轴列和 Y 轴列，程序用所选数值直接绘图。各轴可设置单位换算和刻度小数位。如果 X 列以秒为单位，可选择「秒 → 分钟」或「秒 → 小时」。

```csv
Time_s,Voltage[V]
0,1.2
30,2.1
60,3.3
```

### PWM 数据

选择「原始 PWM 方波数据」，指定时间列和信号列。输入可为数字电平、采样电压，也可为已有的占空比读数。

- **电平或电压采样**。选择「高有效」或「低有效」。程序用所选有效电平在完整周期中的时长计算占空比。电平阈值留空时自动判断，也可输入数值阈值。
- **已有占空比**。例如包含 `Time[ns]` 和 `PWM: Duty cycle` 的采集文件。选择「低有效」时，按 `100% − 高有效占空比` 换算。
- **时间与精度**。「输入时间单位」表示 CSV 时间列的原始单位。「显示单位」决定图上用秒、分钟还是小时。「PWM 分组」默认是 0.1 秒，程序对同组的占空比取平均。

下面是以微秒为单位的数字电平示例。选择高有效时，完整周期的占空比为 30%。

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

## 查看和保存

| 操作 | 用法 |
| --- | --- |
| 缩放 | 在绘图区滚动鼠标滚轮，或点击「+」和「−」。 |
| 平移 | 按住绘图区内的鼠标左键拖动，松手后更新视图。 |
| 恢复全图 | 点击「全图」。 |
| 查看坐标 | 将指针移入绘图区，十字线旁显示当前位置的 X、Y 读数和标注单位。 |
| 导出图片 | 点击「保存 JPG」，保存当前缩放和平移后的图。 |
| 导出 PWM 数据 | 在 PWM 模式生成曲线后点击「保存转换 CSV」。输出包含 `Id`、`Time_s` 和 `Duty_%`，时间列以秒为单位。 |

## 从源码构建

使用 Visual Studio 2022 的 MSVC v143 C++ 工具链和 CMake。以下命令生成 32 位 Windows 7 版本。

```powershell
cmake -S . -B build-x86 -G "Visual Studio 17 2022" -A Win32 -DCSV_CURVE_WIN7=ON
cmake --build build-x86 --config Release --target csv_curve
```

构建产物位于 `build-x86/Release/csv_curve.exe`。构建 64 位版时使用 `-A x64`，并另选构建目录。

## 开源许可证

本项目采用 [MIT 许可证](./LICENSE)。
