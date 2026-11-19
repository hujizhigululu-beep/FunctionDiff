#include "compare.h"
#include <stdint.h>

typedef unsigned short W;
typedef void *Handle;
typedef uint32_t Dword;
typedef uintptr_t WP;
typedef intptr_t LP;

#define API(name, result, args) IMPORT(name, result, args)
#define INVALID_HANDLE ((Handle)(intptr_t)-1)
#define FILE_DIRECTORY 16u
#define FILE_REPARSE_POINT 1024u
#define ERROR_NO_MORE_FILES 18u
#define WM_COMMAND 0x0111u
#define WM_CREATE 1u
#define WM_DESTROY 2u
#define WM_PAINT 15u
#define WM_DRAWITEM 0x002bu
#define WM_CTLCOLOREDIT 0x0133u
#define WM_CTLCOLORSTATIC 0x0138u
#define WS_CHILD 0x40000000u
#define WS_VISIBLE 0x10000000u
#define WS_TABSTOP 0x00010000u
#define RGB(r,g,b) ((Dword)((r) | (g) << 8 | (b) << 16))
#define COLOR_CANVAS RGB(240,245,248)
#define COLOR_CARD RGB(255,255,255)
#define COLOR_HEADER RGB(23,48,68)
#define COLOR_TEXT RGB(22,45,61)
#define COLOR_MUTED RGB(70,103,119)
#define COLOR_PRIMARY RGB(18,98,95)

__attribute__((section(".winimp"))) void * volatile win_imports[512] = {(void *)1};
void * volatile *win_iat = win_imports;

typedef struct {
    Dword flags, times[6], size_high, size_low, reserved[2];
    W name[260], alternate[14];
} FindData;

typedef struct {
    Dword size, style;
    LP (*procedure)(Handle, Dword, WP, LP);
    int class_extra, window_extra;
    Handle instance, icon, cursor, background;
    const W *menu, *name;
    Handle small_icon;
} WindowClass;

typedef struct {
    Handle window;
    Dword message;
    WP wparam;
    LP lparam;
    Dword time;
    int x, y;
    Dword private_data;
} Message;

typedef struct { int left, top, right, bottom; } Rect;
typedef struct { Handle dc; int erase; Rect rect; int restore, update; unsigned char reserved[32]; } Paint;
typedef struct {
    unsigned type, id, item, action, state;
    Handle control, dc;
    Rect rect;
    WP data;
} DrawItem;

typedef struct {
    Dword size;
    Handle owner, instance;
    const W *filter;
    W *custom_filter;
    Dword max_custom_filter, filter_index;
    W *file;
    Dword max_file;
    W *file_title;
    Dword max_file_title;
    const W *initial_dir, *title;
    Dword flags;
    unsigned short file_offset, file_extension;
    const W *default_extension;
    LP custom_data;
    void *hook;
    const W *template_name;
    void *reserved;
    Dword reserved_size, extended_flags;
} FileDialog;

typedef struct { Dword first; uint16_t second, third; unsigned char tail[8]; } Guid;
typedef struct { void **vtable; } ComObject;
typedef int32_t Result;
enum {
    COM_RELEASE = 2, DIALOG_SHOW = 3, DIALOG_SET_OPTIONS = 9, DIALOG_GET_OPTIONS = 10,
    DIALOG_SET_TITLE = 17, DIALOG_GET_RESULT = 20, SHELL_ITEM_GET_PATH = 5
};
#define COM(object, slot, result, signature) ((result (*) signature) (object)->vtable[slot])

_Static_assert(sizeof(FindData) == 592 && sizeof(WindowClass) == 80, "unexpected Win64 layout");
_Static_assert(sizeof(Message) == 48 && sizeof(FileDialog) == 152 && sizeof(Guid) == 16,
               "unexpected Win64 dialog layout");
_Static_assert(sizeof(Paint) == 72 && sizeof(DrawItem) == 64, "unexpected Win64 paint layout");

enum { OLD_FILE = 101, OLD_FOLDER, NEW_FILE, NEW_FOLDER, EXPORT_HTML, EXPORT_JSON, EXPORT_JSON_AFTER };
static Handle instance, window, old_edit, new_edit, status_label, font;
static Handle export_buttons[3];
static Handle title_font, small_font, app_icon, caption_icon, header_brush, card_brush, canvas_brush;

static size_t wlength(const W *text)
{
    size_t size = 0;
    while (text[size]) size++;
    return size;
}

static W *to_wide(const char *text, size_t length, Dword page, Dword flags, size_t *wide_size)
{
    if (length > 0x7fffffffu) return 0;
    int (*convert)(Dword, Dword, const char *, int, W *, int) =
        API(MultiByteToWideChar, int, (Dword, Dword, const char *, int, W *, int));
    int count = length ? convert(page, flags, text, (int)length, 0, 0) : 0;
    if (!count && length) return 0;
    W *result = grow_array(0, ((size_t)count + 1) * sizeof(W));
    if (count && !convert(page, flags, text, (int)length, result, count)) {
        free(result);
        return 0;
    }
    result[count] = 0;
    if (wide_size) *wide_size = (size_t)count;
    return result;
}

static W *utf8_wide(const char *text)
{
    return to_wide(text, strlen(text), 65001, 8, 0);
}

static char *from_wide(const W *text, size_t length, size_t *new_size)
{
    if (length > 0x7fffffffu) return 0;
    int (*convert)(Dword,Dword,const W *,int,char *,int,const char *,int *) =
        API(WideCharToMultiByte, int, (Dword,Dword,const W *,int,char *,int,const char *,int *));
    int count = length ? convert(65001, 0, text, (int)length, 0, 0, 0, 0) : 0;
    if (length && !count) return 0;
    char *utf8 = grow_array(0, (size_t)count + 1);
    if (count && !convert(65001, 0, text, (int)length, utf8, count, 0, 0)) {
        free(utf8);
        return 0;
    }
    utf8[count] = 0;
    if (new_size) *new_size = (size_t)count;
    return utf8;
}

char *convert_utf8(char *bytes, size_t size, size_t *new_size)
{
    const unsigned char *raw = (const unsigned char *)bytes;
    if (size >= 3 && raw[0] == 0xef && raw[1] == 0xbb && raw[2] == 0xbf) {
        for (size_t i = 3; i < size; i++) bytes[i - 3] = bytes[i];
        *new_size = size - 3;
        return bytes;
    }
    size_t units = 0;
    W *wide = 0;
    if (size >= 2 && ((raw[0] == 0xff && raw[1] == 0xfe) || (raw[0] == 0xfe && raw[1] == 0xff))) {
        int big_endian = raw[0] == 0xfe;
        if (size % 2) { free(bytes); return 0; }
        units = (size - 2) / 2;
        wide = grow_array(0, (units + 1) * sizeof(W));
        for (size_t i = 0; i < units; i++) {
            size_t offset = 2 + i * 2;
            wide[i] = (W)(raw[offset + big_endian] | (raw[offset + !big_endian] << 8));
        }
        wide[units] = 0;
    } else {
        wide = to_wide(bytes, size, 65001, 8, &units);
        if (wide) {
            free(wide);
            *new_size = size;
            return bytes;
        }
        wide = to_wide(bytes, size, 936, 8, &units);
    }
    char *converted = wide ? from_wide(wide, units, new_size) : 0;
    free(wide);
    free(bytes);
    return converted;
}

void memory_error(void)
{
    API(MessageBoxW, int, (Handle,const W *,const W *,Dword))(window, L"内存不足，比较已停止。", L"函数差异导出", 16);
    API(ExitProcess, void, (Dword))(2);
    __builtin_unreachable();
}

int path_kind(const char *path)
{
    W *wide = utf8_wide(path);
    if (!wide) return 0;
    Dword mode = API(GetFileAttributesW,Dword,(const W *))(wide);
    free(wide);
    if (mode == 0xffffffffu) return 0;
    return mode & FILE_DIRECTORY ? 2 : 1;
}

int collect_paths(const char *root, const char *relative, Paths *paths)
{
    char *directory = join_path(root, relative);
    char *pattern = join_path(directory, "*");
    W *wide = utf8_wide(pattern);
    free(directory);
    free(pattern);
    if (!wide) return 0;
    FindData record;
    Handle found = API(FindFirstFileW,Handle,(const W *,FindData *))(wide, &record);
    Dword find_error = found == INVALID_HANDLE ? API(GetLastError,Dword,(void))() : 0;
    free(wide);
    if (found == INVALID_HANDLE)
        return find_error == 2u; /* empty directory */
    int okay = 1;
    do {
        char *name = from_wide(record.name, wlength(record.name), 0);
        if (!name) { okay = 0; break; }
        if (strcmp(name, ".") && strcmp(name, "..") && strcmp(name, ".git") &&
            !(record.flags & FILE_REPARSE_POINT)) {
            char *next = join_path(relative, name);
            if (record.flags & FILE_DIRECTORY) okay = collect_paths(root, next, paths);
            else if (is_source(name)) add_path(paths, next);
            free(next);
        }
        free(name);
        if (!okay) break;
    } while (API(FindNextFileW,int,(Handle,FindData *))(found, &record));
    if (okay) okay = API(GetLastError,Dword,(void))() == ERROR_NO_MORE_FILES;
    API(FindClose,int,(Handle))(found);
    return okay;
}

void *open_path(const char *path, int writing)
{
    W *wide = utf8_wide(path);
    if (!wide) return 0;
    void *file = API(_wfopen,void *,(const W *,const W *))(wide, writing ? L"wb" : L"rb");
    free(wide);
    return file;
}
size_t read_data(void *file, char *buffer, size_t size)
{
    return API(fread,size_t,(void *,size_t,size_t,void *))(buffer, 1, size, file);
}
int read_error(void *file) { return API(ferror,int,(void *))(file); }
int write_data(void *file, const char *buffer, size_t size)
{
    return API(fwrite,size_t,(const void *,size_t,size_t,void *))(buffer, 1, size, file) == size;
}
int close_path(void *file) { return API(fclose,int,(void *))(file) == 0; }

static void set_text(Handle control, const W *text)
{
    API(SetWindowTextW,int,(Handle,const W *))(control, text);
}

static int message(const W *text, Dword flags)
{
    return API(MessageBoxW,int,(Handle,const W *,const W *,Dword))(window, text, L"C 函数差异导出", flags);
}

static W *read_control(Handle control)
{
    W *text = grow_array(0, 32768 * sizeof(W));
    int length = API(GetWindowTextW,int,(Handle,W *,int))(control, text, 32768);
    if (length > 1 && text[0] == '"' && text[length - 1] == '"') {
        for (int i = 0; i < length - 2; i++) text[i] = text[i + 1];
        text[length - 2] = 0;
    }
    return text;
}

static void select_folder(Handle edit, const W *title)
{
    static const Guid picker_class =
        {0xdc1c5a9c, 0xe88a, 0x4dde, {0xa5,0xa1,0x60,0xf8,0x2a,0x20,0xae,0xf7}};
    static const Guid picker_interface =
        {0xd57c7288, 0xd4ad, 0x4768, {0xbe,0x02,0x9d,0x96,0x95,0x32,0xd9,0x60}};
    ComObject *picker = 0, *item = 0;
    W *path = 0;
    Result status = API(CoCreateInstance,Result,(const Guid *,void *,Dword,const Guid *,ComObject **))
        (&picker_class, 0, 1, &picker_interface, &picker);
    if (status >= 0) {
        Dword options = 0;
        status = COM(picker,DIALOG_GET_OPTIONS,Result,(ComObject *,Dword *))(picker, &options);
        if (status >= 0)
            status = COM(picker,DIALOG_SET_OPTIONS,Result,(ComObject *,Dword))
                (picker, options | 0x00000020u | 0x00000040u | 0x00000800u);
        if (status >= 0) {
            COM(picker,DIALOG_SET_TITLE,Result,(ComObject *,const W *))(picker, title);
            status = COM(picker,DIALOG_SHOW,Result,(ComObject *,Handle))(picker, window);
        }
        if (status >= 0)
            status = COM(picker,DIALOG_GET_RESULT,Result,(ComObject *,ComObject **))(picker, &item);
        if (status >= 0)
            status = COM(item,SHELL_ITEM_GET_PATH,Result,(ComObject *,Dword,W **))(item, 0x80058000u, &path);
    }
    if (path) {
        if (status >= 0) set_text(edit, path);
        API(CoTaskMemFree,void,(void *))(path);
    }
    if (item) COM(item,COM_RELEASE,Dword,(ComObject *))(item);
    if (picker) COM(picker,COM_RELEASE,Dword,(ComObject *))(picker);
    if (status < 0 && (Dword)status != 0x800704c7u)
        message(L"文件夹选择对话框不可用。也可以把文件夹完整路径粘贴到主窗口。", 48);
}

static int select_file(W *path, int saving, int json)
{
    static const W source_filter[] = L"C 源文件和头文件 (*.c;*.h)\0*.c;*.h\0\0";
    static const W html_filter[] = L"HTML 报告 (*.html)\0*.html\0\0";
    static const W json_filter[] = L"JSON 数据 (*.json)\0*.json\0\0";
    FileDialog state = {0};
    state.size = sizeof(state);
    state.owner = window;
    state.filter = saving ? json ? json_filter : html_filter : source_filter;
    state.filter_index = 1;
    state.file = path;
    state.max_file = 32768;
    state.flags = 0x00000800u | 0x00000008u | (saving ? 0x2u : 0x00001000u);
    state.title = saving ? L"选择报告的保存位置" : L"选择 C 源文件或头文件";
    state.default_extension = saving ? json ? L"json" : L"html" : 0;
    int okay = saving ? API(GetSaveFileNameW,int,(FileDialog *))(&state) :
        API(GetOpenFileNameW,int,(FileDialog *))(&state);
    if (!okay && API(CommDlgExtendedError,Dword,(void))())
        message(L"文件对话框无法打开。可以把路径直接填到主窗口。", 48);
    return okay;
}

static void choose_source(Handle edit)
{
    W *path = grow_array(0, 32768 * sizeof(W));
    path[0] = 0;
    if (select_file(path, 0, 0)) set_text(edit, path);
    free(path);
}

static void show_problem(const char *why, const char *path)
{
    char *full = path ? join_path(why, path) : 0;
    W *wide = utf8_wide(full ? full : why);
    if (full) {
        size_t split = strlen(why);
        full[split] = '\n';
        free(wide);
        wide = utf8_wide(full);
    }
    message(wide ? wide : L"比较失败。", 48);
    free(wide);
    free(full);
}

static size_t append_wide(W *result, size_t used, const W *text)
{
    while (*text) result[used++] = *text++;
    return used;
}

static size_t append_count(W *result, size_t used, size_t number)
{
    W digits[32];
    size_t next = 32;
    do {
        digits[--next] = (W)(L'0' + number % 10);
        number /= 10;
    } while (number);
    while (next < 32) result[used++] = digits[next++];
    return used;
}

static void show_counts(const Summary *result, ReportFormat format)
{
    W label[120];
    size_t used = append_wide(label, 0, L"已比较 ");
    used = append_count(label, used, result->compared);
    used = append_wide(label, used, L" 个源码文件，其中 ");
    used = append_count(label, used, result->files);
    used = append_wide(label, used, format == REPORT_JSON_AFTER
        ? L" 个包含新增或修改的函数。新版 JSON 已导出。" : L" 个有差异。报告已导出。");
    label[used] = 0;
    set_text(status_label, label);
}

static void export_report(ReportFormat format)
{
    int json = format != REPORT_HTML;
    W *left = read_control(old_edit), *right = read_control(new_edit);
    if (!left[0] || !right[0]) {
        message(L"请先选择旧版和新版源码。", 48);
        free(left);
        free(right);
        return;
    }
    W *chosen = grow_array(0, 32768 * sizeof(W));
    const W *default_names[] = {L"函数差异.html", L"函数差异.json", L"变更函数-仅新版.json"};
    const W *default_name = default_names[format];
    memcpy(chosen, default_name, (wlength(default_name) + 1) * sizeof(W));
    if (select_file(chosen, 1, json)) {
        char *output = from_wide(chosen, wlength(chosen), 0);
        char *old_path = from_wide(left, wlength(left), 0);
        char *new_path = from_wide(right, wlength(right), 0);
        if (!output || !old_path || !new_path) {
            set_text(status_label, L"路径编码有误");
            message(L"无法读取所选路径。", 48);
        } else if (is_source(output)) {
            message(L"请将报告保存为 .html 或 .json 文件。", 48);
        } else {
            const char *extension = json ? ".json" : ".html";
            size_t suffix = strlen(extension), n = strlen(output);
            int extension_ok = n > suffix && path_compare(output + n - suffix, extension) == 0;
            if (!extension_ok) {
                message(json ? L"请使用 .json 后缀。" : L"请使用 .html 后缀。", 48);
            } else {
                size_t directory_size = (size_t)(base_name(output) - output);
                char *directory = directory_size ? copy_bytes(output, directory_size) : copy_bytes(".", 1);
                W *wide_dir = utf8_wide(directory);
                W temporary[260];
                set_text(status_label, L"正在比较源码…");
                API(UpdateWindow,int,(Handle))(window);
                if (!wide_dir || !API(GetTempFileNameW,unsigned int,(const W *,const W *,unsigned int,W *))
                    (wide_dir, L"FDF", 0, temporary)) {
                    set_text(status_label, L"无法写入保存目录");
                    message(L"无法在保存目录创建临时报告。", 48);
                } else {
                    char *stage = from_wide(temporary, wlength(temporary), 0);
                    Summary statistics = {0};
                    int okay = stage && compare_paths(old_path, new_path, stage, format, &statistics);
                    if (okay) okay = API(MoveFileExW,int,(const W *,const W *,Dword))(temporary, chosen, 9u);
                    if (!okay) {
                        API(DeleteFileW,int,(const W *))(temporary);
                        set_text(status_label, L"报告未导出");
                        show_problem(statistics.problem ? statistics.problem : "无法保存报告，请确认文件未被占用",
                                     statistics.problem_path);
                    } else {
                        show_counts(&statistics, format);
                        if (!json && message(L"报告已保存。现在用浏览器打开？", 4u | 64u) == 6)
                            API(ShellExecuteW,Handle,(Handle,const W *,const W *,const W *,const W *,int))
                                (window, L"open", chosen, 0, 0, 1);
                        else if (json) message(format == REPORT_JSON_AFTER
                            ? L"新版 JSON 已保存。" : L"JSON 报告已保存。", 64);
                    }
                    free(statistics.problem_path);
                    free(stage);
                }
                free(wide_dir);
                free(directory);
            }
        }
        free(output);
        free(old_path);
        free(new_path);
    }
    free(chosen);
    free(left);
    free(right);
}

static Handle control(const W *class_name, const W *title, Dword styles,
                      int x, int y, int width, int height, unsigned id)
{
    Handle result = API(CreateWindowExW,Handle,
        (Dword,const W *,const W *,Dword,int,int,int,int,Handle,Handle,Handle,void *))
        (class_name[0] == L'E' ? 0x200u : 0, class_name, title,
         WS_CHILD | WS_VISIBLE | styles, x, y, width, height, window,
         (Handle)(WP)id, instance, 0);
    if (font) API(SendMessageW,LP,(Handle,Dword,WP,LP))(result, 0x30u, (WP)font, 1);
    return result;
}

static Handle ui_font(int size, int weight)
{
    return API(CreateFontW,Handle,(int,int,int,int,int,Dword,Dword,Dword,Dword,Dword,Dword,Dword,Dword,const W *))
        (-size, 0, 0, 0, weight, 0, 0, 0, 1, 0, 0, 5, 0, L"Microsoft YaHei UI");
}

static void rectangle(Handle dc, Rect bounds, Handle brush)
{
    API(FillRect,int,(Handle,const Rect *,Handle))(dc, &bounds, brush);
}

static void paint_text(Handle dc, const W *text, Rect bounds, Handle typeface, Dword color, unsigned align)
{
    Handle previous = typeface ? API(SelectObject,Handle,(Handle,Handle))(dc, typeface) : 0;
    API(SetBkMode,int,(Handle,int))(dc, 1);
    API(SetTextColor,Dword,(Handle,Dword))(dc, color);
    API(DrawTextW,int,(Handle,const W *,int,Rect *,unsigned))(dc, text, -1, &bounds, align | 0x824u);
    if (previous) API(SelectObject,Handle,(Handle,Handle))(dc, previous);
}

static void paint_window(Handle hwnd)
{
    Paint paint;
    Handle dc = API(BeginPaint,Handle,(Handle,Paint *))(hwnd, &paint);
    if (dc) {
        rectangle(dc, (Rect){0, 0, 740, 555}, canvas_brush);
        rectangle(dc, (Rect){0, 0, 740, 105}, header_brush);
        if (app_icon)
            API(DrawIconEx,int,(Handle,int,int,Handle,int,int,unsigned,Handle,unsigned))
                (dc, 25, 21, app_icon, 62, 62, 0, 0, 3);
        paint_text(dc, L"FunctionDiff", (Rect){105, 18, 680, 58}, title_font, COLOR_CARD, 0);
        paint_text(dc, L"C 源码函数对比 · HTML / JSON", (Rect){106, 61, 680, 85},
                   small_font, RGB(185,215,223), 0);
        rectangle(dc, (Rect){20, 122, 700, 220}, card_brush);
        rectangle(dc, (Rect){20, 233, 700, 331}, card_brush);
        rectangle(dc, (Rect){20, 345, 700, 455}, card_brush);
        paint_text(dc, L"旧版源码", (Rect){37, 132, 167, 157}, font, COLOR_TEXT, 0);
        paint_text(dc, L"新版源码", (Rect){37, 243, 167, 268}, font, COLOR_TEXT, 0);
        paint_text(dc, L"导出报告", (Rect){37, 355, 167, 381}, font, COLOR_TEXT, 0);
        paint_text(dc, L"新版 JSON 仅含新增和修改后的函数 · 导出内容默认脱敏", (Rect){37, 428, 683, 450},
                   small_font, COLOR_MUTED, 0);
    }
    API(EndPaint,int,(Handle,const Paint *))(hwnd, &paint);
}

static void paint_button(const DrawItem *item)
{
    int primary = item->id == EXPORT_HTML;
    int inactive = (item->state & 4u) != 0;
    Dword background = inactive ? RGB(226,234,237) :
        primary ? ((item->state & 1u) ? RGB(12,76,73) : COLOR_PRIMARY) :
        ((item->state & 1u) ? RGB(204,225,231) : RGB(224,239,242));
    Handle brush = API(CreateSolidBrush,Handle,(Dword))(background);
    rectangle(item->dc, item->rect, brush);
    W label[80] = {0};
    API(GetWindowTextW,int,(Handle,W *,int))(item->control, label, 80);
    paint_text(item->dc, label, item->rect, font, primary && !inactive ? COLOR_CARD : COLOR_TEXT, 1);
    if (item->state & 0x10u) {
        Rect focus = {item->rect.left + 4, item->rect.top + 4, item->rect.right - 4, item->rect.bottom - 4};
        API(DrawFocusRect,int,(Handle,const Rect *))(item->dc, &focus);
    }
    if (brush) API(DeleteObject,int,(Handle))(brush);
}

static LP window_proc(Handle hwnd, Dword what, WP wparam, LP lparam)
{
    if (what == WM_CREATE) {
        window = hwnd;
        font = ui_font(16, 500);
        small_font = ui_font(14, 400);
        title_font = ui_font(32, 700);
        const Dword button = WS_TABSTOP | 11u; /* BS_OWNERDRAW */
        old_edit = control(L"EDIT", L"", WS_TABSTOP | 0x80u, 37, 167, 410, 35, 0);
        control(L"BUTTON", L"选文件夹…", button, 460, 167, 122, 35, OLD_FOLDER);
        control(L"BUTTON", L"单文件…", button, 592, 167, 90, 35, OLD_FILE);
        new_edit = control(L"EDIT", L"", WS_TABSTOP | 0x80u, 37, 278, 410, 35, 0);
        control(L"BUTTON", L"选文件夹…", button, 460, 278, 122, 35, NEW_FOLDER);
        control(L"BUTTON", L"单文件…", button, 592, 278, 90, 35, NEW_FILE);
        export_buttons[REPORT_HTML] = control(L"BUTTON", L"导出 HTML", button, 37, 386, 202, 38, EXPORT_HTML);
        export_buttons[REPORT_JSON] = control(L"BUTTON", L"导出对比 JSON", button, 255, 386, 202, 38, EXPORT_JSON);
        export_buttons[REPORT_JSON_AFTER] = control(L"BUTTON", L"导出新版 JSON", button, 473, 386, 209, 38, EXPORT_JSON_AFTER);
        status_label = control(L"STATIC", L"选择旧版和新版文件夹，也可以直接粘贴路径。", 0, 23, 473, 675, 25, 0);
        return 0;
    }
    if (what == WM_PAINT) { paint_window(hwnd); return 0; }
    if (what == WM_DRAWITEM) { paint_button((const DrawItem *)lparam); return 1; }
    if (what == WM_CTLCOLOREDIT || what == WM_CTLCOLORSTATIC) {
        int edit = what == WM_CTLCOLOREDIT;
        API(SetTextColor,Dword,(Handle,Dword))((Handle)wparam, edit ? COLOR_TEXT : COLOR_MUTED);
        API(SetBkColor,Dword,(Handle,Dword))((Handle)wparam, edit ? COLOR_CARD : COLOR_CANVAS);
        return (LP)(edit ? card_brush : canvas_brush);
    }
    if (what == WM_COMMAND) {
        switch (wparam & 0xffffu) {
        case OLD_FILE: choose_source(old_edit); return 0;
        case OLD_FOLDER: select_folder(old_edit, L"选择旧版源码文件夹"); return 0;
        case NEW_FILE: choose_source(new_edit); return 0;
        case NEW_FOLDER: select_folder(new_edit, L"选择新版源码文件夹"); return 0;
        case EXPORT_HTML:
        case EXPORT_JSON:
        case EXPORT_JSON_AFTER:
            for (size_t i = 0; i < sizeof(export_buttons) / sizeof(*export_buttons); i++)
                API(EnableWindow,int,(Handle,int))(export_buttons[i], 0);
            export_report((ReportFormat)((wparam & 0xffffu) - EXPORT_HTML));
            for (size_t i = 0; i < sizeof(export_buttons) / sizeof(*export_buttons); i++)
                API(EnableWindow,int,(Handle,int))(export_buttons[i], 1);
            return 0;
        default: break;
        }
    }
    if (what == WM_DESTROY) {
        if (font) API(DeleteObject,int,(Handle))(font);
        if (small_font) API(DeleteObject,int,(Handle))(small_font);
        if (title_font) API(DeleteObject,int,(Handle))(title_font);
        API(PostQuitMessage,void,(int))(0);
        return 0;
    }
    return API(DefWindowProcW,LP,(Handle,Dword,WP,LP))(hwnd, what, wparam, lparam);
}

void app_entry(void)
{
    instance = API(GetModuleHandleW,Handle,(const W *))(0);
    int ole_status = API(CoInitializeEx,int,(void *,Dword))(0, 2);
    WindowClass cls = {0};
    cls.size = sizeof(cls);
    cls.procedure = window_proc;
    cls.instance = instance;
    Handle (*load_icon)(Handle,const W *,unsigned,int,int,unsigned) =
        API(LoadImageW,Handle,(Handle,const W *,unsigned,int,int,unsigned));
    app_icon = load_icon(instance, (const W *)(WP)1, 1, 64, 64, 0);
    caption_icon = load_icon(instance, (const W *)(WP)1, 1, 16, 16, 0);
    cls.icon = app_icon;
    cls.small_icon = caption_icon;
    header_brush = API(CreateSolidBrush,Handle,(Dword))(COLOR_HEADER);
    card_brush = API(CreateSolidBrush,Handle,(Dword))(COLOR_CARD);
    canvas_brush = API(CreateSolidBrush,Handle,(Dword))(COLOR_CANVAS);
    cls.cursor = API(LoadCursorW,Handle,(Handle,const W *))(0, (const W *)(WP)32512);
    cls.background = canvas_brush;
    cls.name = L"FunctionDiffWindow";
    if (API(RegisterClassExW,unsigned short,(const WindowClass *))(&cls)) {
        window = API(CreateWindowExW,Handle,
            (Dword,const W *,const W *,Dword,int,int,int,int,Handle,Handle,Handle,void *))
            (0, cls.name, L"FunctionDiff · C 函数差异导出", 0x00ca0000u, (int)0x80000000u, 0, 738, 555,
             0, 0, instance, 0);
    }
    if (!window) {
        message(L"无法创建窗口。", 16);
        API(ExitProcess,void,(Dword))(1);
    }
    API(ShowWindow,int,(Handle,int))(window, 5);
    API(UpdateWindow,int,(Handle))(window);
    API(SetFocus,Handle,(Handle))(old_edit);
    Message msg;
    int status;
    while ((status = API(GetMessageW,int,(Message *,Handle,Dword,Dword))(&msg, 0, 0, 0)) > 0) {
        if (!API(IsDialogMessageW,int,(Handle,Message *))(window, &msg)) {
            API(TranslateMessage,int,(const Message *))(&msg);
            API(DispatchMessageW,LP,(const Message *))(&msg);
        }
    }
    if (app_icon) API(DestroyIcon,int,(Handle))(app_icon);
    if (caption_icon) API(DestroyIcon,int,(Handle))(caption_icon);
    if (header_brush) API(DeleteObject,int,(Handle))(header_brush);
    if (card_brush) API(DeleteObject,int,(Handle))(card_brush);
    if (ole_status >= 0) API(CoUninitialize,void,(void))();
    API(ExitProcess,void,(Dword))(status < 0 ? 1u : 0u);
    __builtin_unreachable();
}
