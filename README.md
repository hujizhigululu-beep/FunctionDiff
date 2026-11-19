# FunctionDiff

在 Windows 上按函数比较两版 C 源码。可以选择两个 `.c` / `.h` 文件，也可以比较两个文件夹。比较和导出都在本机完成。

## 用法

1. 打开 Windows x64 版 `FunctionDiff.exe`。
2. 选择旧版和新版源码。选择文件夹时，会递归查找 C 源文件和头文件，并按相对路径配对。
3. 选择导出格式。

| 按钮 | 内容 |
| --- | --- |
| 导出 HTML | 旧版和新版完整函数并排，代码差异行用红底标出。 |
| 导出对比 JSON | 每个变更函数含变更类型、两版行号和代码。 |
| 导出新版 JSON | 仅含新增和修改后的函数，代码位于 `afterContent`，可用于圈定单元测试范围。 |

比较时忽略空白和注释差异，保留会改变 C 记号的差异。报告用 `File-1.c` 等编号代替源文件名，省略注释、字符串内容和函数内预处理指令。报告仍保留函数名、变量名和数值。单元测试编译时使用新版工程源码。

## 构建

构建脚本在 x86-64 Linux 上运行，需要 Python 3、Pillow、GCC 和支持 PE x86-64 的 GNU binutils。

~~~sh
python3 -m pip install Pillow
python3 build_windows.py
~~~

输出文件位于 `build/FunctionDiff.exe`。编译依赖用于构建，运行 EXE 时只使用 Windows 系统库。

## 验证

在 Linux 上运行比较核心的回归测试。

~~~sh
gcc -std=c11 -Wall -Wextra -Werror -O2 compare.c qa/linux.c -o qa/compare
python3 qa/check.py
~~~

`qa/pe_smoke.c` 可以在 x86-64 Linux 上用模拟 Win32 接口检查打包后的程序和导出流程。

~~~sh
gcc -std=c11 -Wall -Wextra -Werror -O2 -rdynamic qa/pe_smoke.c -o qa/pe_smoke -ldl
./qa/pe_smoke build/FunctionDiff.exe /path/to/old /path/to/new /tmp/after.json after
~~~

## 许可证

[MIT](LICENSE)
