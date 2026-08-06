#include <windows.h>
#include <shellapi.h>
#include <wchar.h>

#define WM_TRAY (WM_USER + 1)
#define TRAY_ID 1

/* Идентификаторы команд. Диапазоны нумеруются по ИНДЕКСУ пункта, а не по его
   значению, поэтому добавление новых пресетов не может столкнуться с соседним
   диапазоном. Между диапазонами оставлен запас. */
#define ID_TOGGLE      100
#define ID_THR_BASE    1000
#define ID_THR_CUSTOM  1099
#define ID_POST_BASE   1100
#define ID_POST_CUSTOM 1199
#define ID_BTN_BASE    1200
#define ID_AUTO        1300
#define ID_EXIT        1301

#define IDI_APP        101
#define IDD_INPUT      200
#define IDC_EDIT       1001
#define IDC_TEXT       1002

#define HOLD_MS        30u
#define TIMER_ID       1
#define TIMER_MS       1000u
#define TRAY_RETRY_MAX 10
#define HOOK_DEAD_MS   3000u

static const wchar_t* RUN_KEY   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_NAME  = L"AntiDoubleClick";
static const wchar_t* SETUP_KEY = L"Software\\AntiDoubleClick";

static const UINT64 THRESHOLDS[] = {5,10,20,30,50,80,100,200,300};
static const int THR_COUNT = sizeof(THRESHOLDS)/sizeof(THRESHOLDS[0]);

static const UINT64 POST_DELAYS[] = {0,15,20,30,40,50,80,100,200};
static const int POST_COUNT = sizeof(POST_DELAYS)/sizeof(POST_DELAYS[0]);

enum { BTN_L = 0, BTN_R, BTN_M, BTN_COUNT };

static const wchar_t* BTN_NAMES[BTN_COUNT]  = { L"Левая", L"Правая", L"Средняя" };
static const wchar_t  BTN_LETTERS[BTN_COUNT] = { L'Л', L'П', L'С' };

/* Состояние фильтра ведётся отдельно для каждой кнопки: нажатия разных кнопок
   независимы, и общий таймер приводил бы к перекрёстным ложным блокировкам. */
struct BtnState {
    UINT64 last_down;
    UINT64 last_up;
    UINT64 ignore_until;
    UINT64 blocked;
    BOOL   swallow_up;  /* погасить ближайший UP: его DOWN был заблокирован */
    BOOL   app_down;    /* приложение ниже по цепочке видит кнопку нажатой */
};

static BtnState g_btn[BTN_COUNT] = {};
static BOOL     g_btn_on[BTN_COUNT] = { TRUE, FALSE, FALSE };

static BOOL    g_on      = TRUE;
static UINT64  g_thr     = 5;
static UINT64  g_post    = 0;
static HHOOK   g_hook    = NULL;
static HWND    g_hwnd    = NULL;
static HANDLE  g_mutex   = NULL;
static double  g_qpc_freq = 1.0;
static UINT    g_msg_taskbar = 0;
static BOOL    g_tray_ok = FALSE;
static int     g_tray_tries = 0;
static UINT64  g_tip_blocked = (UINT64)-1;
static POINT   g_last_pt = {};

/* Обновляется на каждом событии мыши. Windows молча снимает низкоуровневый хук,
   если колбэк превысил LowLevelHooksTimeout, и узнать об этом иначе нельзя. */
static volatile ULONGLONG g_hook_tick = 0;

static UINT64 now_us() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (UINT64)(li.QuadPart / g_qpc_freq);
}

static void reset_state() {
    for (int i = 0; i < BTN_COUNT; i++) {
        g_btn[i].last_down = 0;
        g_btn[i].last_up = 0;
        g_btn[i].ignore_until = 0;
        g_btn[i].swallow_up = FALSE;
        g_btn[i].app_down = FALSE;
    }
}

static UINT64 total_blocked() {
    UINT64 t = 0;
    for (int i = 0; i < BTN_COUNT; i++) t += g_btn[i].blocked;
    return t;
}

static BOOL read_dword(HKEY key, const wchar_t* name, DWORD* out) {
    DWORD type = 0, val = 0, sz = sizeof(val);
    if (RegQueryValueExW(key, name, NULL, &type, (BYTE*)&val, &sz) != ERROR_SUCCESS)
        return FALSE;
    if (type != REG_DWORD || sz != sizeof(DWORD))
        return FALSE;
    *out = val;
    return TRUE;
}

static void load_settings() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETUP_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;

    DWORD v;
    if (read_dword(key, L"Threshold", &v) && v >= 1 && v <= 1000) g_thr = v;
    if (read_dword(key, L"PostDelay", &v) && v <= 1000)           g_post = v;
    if (read_dword(key, L"Enabled", &v))                          g_on = (v != 0);
    /* Ключ появился позже остальных: у старых конфигураций его нет, и тогда
       остаётся значение по умолчанию - фильтруется только левая кнопка. */
    if (read_dword(key, L"Buttons", &v)) {
        for (int i = 0; i < BTN_COUNT; i++)
            g_btn_on[i] = ((v >> i) & 1u) != 0;
    }
    RegCloseKey(key);
}

static void save_settings() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETUP_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS)
        return;

    DWORD v = (DWORD)g_thr;
    RegSetValueExW(key, L"Threshold", 0, REG_DWORD, (BYTE*)&v, sizeof(v));
    v = (DWORD)g_post;
    RegSetValueExW(key, L"PostDelay", 0, REG_DWORD, (BYTE*)&v, sizeof(v));
    v = g_on ? 1 : 0;
    RegSetValueExW(key, L"Enabled", 0, REG_DWORD, (BYTE*)&v, sizeof(v));
    v = 0;
    for (int i = 0; i < BTN_COUNT; i++) if (g_btn_on[i]) v |= (1u << i);
    RegSetValueExW(key, L"Buttons", 0, REG_DWORD, (BYTE*)&v, sizeof(v));
    RegCloseKey(key);
}

static BOOL is_autostart() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD sz = 0;
        LONG r = RegQueryValueExW(key, RUN_NAME, 0, 0, 0, &sz);
        RegCloseKey(key);
        return (r == ERROR_SUCCESS || r == ERROR_MORE_DATA);
    }
    return FALSE;
}

static void toggle_autostart(HWND parent) {
    BOOL want_on = !is_autostart();

    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        MessageBoxW(parent, L"Нет доступа к ключу автозапуска", L"Anti-Double-Click",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    LONG r;
    if (!want_on) {
        r = RegDeleteValueW(key, RUN_NAME);
    } else {
        /* Путь берётся в кавычки: без них CreateProcess разбирает строку по
           пробелам и может запустить постороннюю программу из каталога выше. */
        wchar_t path[MAX_PATH + 2];
        path[0] = L'"';
        DWORD n = GetModuleFileNameW(NULL, path + 1, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) {
            RegCloseKey(key);
            MessageBoxW(parent, L"Не удалось определить путь к программе", L"Anti-Double-Click",
                        MB_OK | MB_ICONWARNING);
            return;
        }
        path[n + 1] = L'"';
        path[n + 2] = L'\0';
        r = RegSetValueExW(key, RUN_NAME, 0, REG_SZ, (BYTE*)path,
                           (DWORD)((n + 3) * sizeof(wchar_t)));
    }
    RegCloseKey(key);

    if (r != ERROR_SUCCESS)
        MessageBoxW(parent, L"Не удалось изменить автозапуск", L"Anti-Double-Click",
                    MB_OK | MB_ICONWARNING);
}

static int btn_from_msg(WPARAM wp, BOOL* is_down) {
    switch (wp) {
    case WM_LBUTTONDOWN: *is_down = TRUE;  return BTN_L;
    case WM_LBUTTONUP:   *is_down = FALSE; return BTN_L;
    case WM_RBUTTONDOWN: *is_down = TRUE;  return BTN_R;
    case WM_RBUTTONUP:   *is_down = FALSE; return BTN_R;
    case WM_MBUTTONDOWN: *is_down = TRUE;  return BTN_M;
    case WM_MBUTTONUP:   *is_down = FALSE; return BTN_M;
    }
    return -1;
}

static LRESULT CALLBACK mouse_proc(int code, WPARAM wp, LPARAM lp) {
    g_hook_tick = GetTickCount64();

    if (code != HC_ACTION || !g_on)
        return CallNextHookEx(g_hook, code, wp, lp);

    BOOL is_down = FALSE;
    int b = btn_from_msg(wp, &is_down);
    if (b < 0 || !g_btn_on[b])
        return CallNextHookEx(g_hook, code, wp, lp);

    /* Пропуск синтетических событий (LLMHF_INJECTED) сюда напрашивается, но его
       здесь намеренно нет: программы-ремапперы и утилиты производителей мышей
       переотправляют клики заново, и такая проверка отключила бы фильтр целиком
       у тех, кто ими пользуется. */

    BtnState* s = &g_btn[b];
    UINT64 now = now_us();

    /* Ключевой момент: last_down/last_up/ignore_until отражают ФИЗИЧЕСКИЕ события
       и обновляются даже когда событие погашено. Дребезг идёт пачкой, и окно
       должно ехать вместе с ней, иначе хвост пачки выйдет за порог и пролезет.
       Отдельно от этого app_down отслеживает, что видит приложение ниже по
       цепочке, - только для того, чтобы не отдать ему непарное отпускание. */
    if (is_down) {
        /* Защита от «удержания» работает всегда и независимо от порога: это
           отдельный нижний предел, а не часть настройки. Ослабление его до
           значения порога сделало бы фильтр слабее при малых порогах. */
        BOOL block = (g_post > 0 && now < s->ignore_until)
                  || (s->last_down > 0 && now - s->last_down < HOLD_MS * 1000ull)
                  || (s->last_up > 0 && now - s->last_up < g_thr * 1000);

        s->last_down = now;

        if (block) {
            s->blocked++;
            s->swallow_up = TRUE;
            return 1;
        }
        s->app_down = TRUE;
    } else {
        /* Гасим отпускание только если ниже по цепочке нет непарного нажатия,
           иначе кнопка «залипнет» в чужом приложении. */
        BOOL swallow = s->swallow_up && !s->app_down;
        s->swallow_up = FALSE;

        s->last_up = now;
        s->ignore_until = now + g_post * 1000;

        if (swallow)
            return 1;
        s->app_down = FALSE;
    }

    return CallNextHookEx(g_hook, code, wp, lp);
}

static BOOL install_hook() {
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, GetModuleHandleW(NULL), 0);
    g_hook_tick = GetTickCount64();
    return g_hook != NULL;
}

static void fill_tip(NOTIFYICONDATAW* nid, const wchar_t* tip) {
    wcsncpy_s(nid->szTip, tip, _TRUNCATE);
}

static void refresh_tip() {
    if (!g_tray_ok) return;

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_TIP;

    wchar_t buf[256];
    if (!g_hook) {
        wcscpy_s(buf, L"ADC | хук не установлен");
    } else {
        wchar_t btns[BTN_COUNT + 1];
        int n = 0;
        for (int i = 0; i < BTN_COUNT; i++)
            if (g_btn_on[i]) btns[n++] = BTN_LETTERS[i];
        btns[n] = L'\0';

        swprintf_s(buf, L"ADC | %s | %llu/%llu мс | %s | Блок: %llu",
                   g_on ? L"Вкл" : L"Выкл", g_thr, g_post,
                   n ? btns : L"нет кнопок", total_blocked());
    }
    fill_tip(&nid, buf);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static BOOL add_tray() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP));
    if (!nid.hIcon) nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    fill_tip(&nid, L"Anti-Double-Click");
    return Shell_NotifyIconW(NIM_ADD, &nid);
}

static void remove_tray() {
    if (!g_tray_ok) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_tray_ok = FALSE;
}

/* При автозапуске программа нередко стартует раньше, чем готова область
   уведомлений, и NIM_ADD тихо падает - тогда приложение работает невидимо. */
static void try_add_tray() {
    if (g_tray_ok) return;
    if (add_tray()) {
        g_tray_ok = TRUE;
        g_tip_blocked = (UINT64)-1;
        refresh_tip();
    }
}

struct InputCtx {
    UINT64 initial;
    UINT64 min;
    UINT64 max;
    const wchar_t* title;
    UINT64 result;
    BOOL ok;
};

static INT_PTR CALLBACK input_dlg_proc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        InputCtx* ctx = (InputCtx*)lp;
        SetWindowLongPtrW(hdlg, GWLP_USERDATA, (LONG_PTR)ctx);
        SetWindowTextW(hdlg, ctx->title);
        SetDlgItemTextW(hdlg, IDC_TEXT, L"Значение в мс:");
        SetDlgItemTextW(hdlg, IDCANCEL, L"Отмена");
        SendDlgItemMessageW(hdlg, IDC_EDIT, EM_SETLIMITTEXT, 10, 0);
        wchar_t buf[32];
        swprintf_s(buf, L"%llu", ctx->initial);
        SetDlgItemTextW(hdlg, IDC_EDIT, buf);
        SendDlgItemMessageW(hdlg, IDC_EDIT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(hdlg, IDC_EDIT));
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            InputCtx* ctx = (InputCtx*)GetWindowLongPtrW(hdlg, GWLP_USERDATA);
            wchar_t buf[32];
            GetDlgItemTextW(hdlg, IDC_EDIT, buf, 32);
            wchar_t* end = NULL;
            UINT64 v = wcstoull(buf, &end, 10);
            if (end == buf || *end != L'\0' || v < ctx->min || v > ctx->max) {
                wchar_t err[80];
                swprintf_s(err, L"Введите число от %llu до %llu", ctx->min, ctx->max);
                MessageBoxW(hdlg, err, L"Ошибка", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            ctx->result = v;
            ctx->ok = TRUE;
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static BOOL input_value(HWND parent, const wchar_t* title, UINT64 initial, UINT64 min, UINT64 max, UINT64* out) {
    InputCtx ctx = { initial, min, max, title, 0, FALSE };
    INT_PTR r = DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_INPUT), parent, input_dlg_proc, (LPARAM)&ctx);
    if (r == IDOK && ctx.ok) { *out = ctx.result; return TRUE; }
    return FALSE;
}

static BOOL is_preset(const UINT64* arr, int count, UINT64 v) {
    for (int i = 0; i < count; i++) if (arr[i] == v) return TRUE;
    return FALSE;
}

static HMENU build_preset_menu(const UINT64* arr, int count, UINT64 current,
                               UINT id_base, UINT id_custom, BOOL zero_is_off) {
    HMENU sub = CreatePopupMenu();
    if (!sub) return NULL;

    for (int i = 0; i < count; i++) {
        wchar_t label[32];
        if (zero_is_off && arr[i] == 0)
            wcscpy_s(label, L"Выкл");
        else
            swprintf_s(label, L"%llu мс", arr[i]);
        AppendMenuW(sub, MF_STRING | (current == arr[i] ? MF_CHECKED : 0), id_base + i, label);
    }

    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);

    wchar_t custom[48];
    BOOL is_custom = !is_preset(arr, count, current);
    if (is_custom)
        swprintf_s(custom, L"Своё значение... (%llu мс)", current);
    else
        wcscpy_s(custom, L"Своё значение...");
    AppendMenuW(sub, MF_STRING | (is_custom ? MF_CHECKED : 0), id_custom, custom);

    return sub;
}

static void show_menu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING, ID_TOGGLE, g_on ? L"Выключить" : L"Включить");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    HMENU thr = build_preset_menu(THRESHOLDS, THR_COUNT, g_thr, ID_THR_BASE, ID_THR_CUSTOM, FALSE);
    if (thr) AppendMenuW(menu, MF_POPUP, (UINT_PTR)thr, L"Порог");

    HMENU post = build_preset_menu(POST_DELAYS, POST_COUNT, g_post, ID_POST_BASE, ID_POST_CUSTOM, TRUE);
    if (post) AppendMenuW(menu, MF_POPUP, (UINT_PTR)post, L"Пост-задержка");

    HMENU btns = CreatePopupMenu();
    if (btns) {
        for (int i = 0; i < BTN_COUNT; i++)
            AppendMenuW(btns, MF_STRING | (g_btn_on[i] ? MF_CHECKED : 0), ID_BTN_BASE + i, BTN_NAMES[i]);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)btns, L"Кнопки");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (is_autostart() ? MF_CHECKED : 0), ID_AUTO, L"Автозапуск");
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Выход");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);

    /* TPM_RETURNCMD: без него WM_COMMAND приходит синхронно, пока меню ещё
       активно, и модальный диалог «Своё значение...» открывался бы поверх
       работающего цикла отслеживания меню. */
    UINT cmd = (UINT)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                    pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    /* Подменю, присоединённые через MF_POPUP, уничтожаются вместе с родителем. */
    DestroyMenu(menu);

    if (cmd) SendMessageW(g_hwnd, WM_COMMAND, cmd, 0);
}

static void apply_change() {
    reset_state();
    save_settings();
    refresh_tip();
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g_msg_taskbar && g_msg_taskbar != 0) {
        /* Explorer перезапустился - иконку нужно добавить заново, иначе выйти
           из программы можно будет только через диспетчер задач. */
        g_tray_ok = FALSE;
        g_tray_tries = 0;
        try_add_tray();
        return 0;
    }

    switch (msg) {
    case WM_TRAY:
        if (lp == WM_LBUTTONUP || lp == WM_RBUTTONUP) { show_menu(); }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_ID) {
            if (!g_tray_ok && g_tray_tries < TRAY_RETRY_MAX) {
                g_tray_tries++;
                try_add_tray();
            }
            /* Курсор двигался, а хук об этом не знает - значит система его сняла. */
            POINT pt;
            if (GetCursorPos(&pt)) {
                if ((pt.x != g_last_pt.x || pt.y != g_last_pt.y) &&
                    GetTickCount64() - g_hook_tick > HOOK_DEAD_MS) {
                    install_hook();
                    refresh_tip();
                }
                g_last_pt = pt;
            }
            UINT64 total = total_blocked();
            if (total != g_tip_blocked) {
                g_tip_blocked = total;
                refresh_tip();
            }
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TOGGLE:
            g_on = !g_on;
            apply_change();
            break;
        case ID_THR_CUSTOM: {
            UINT64 v;
            if (input_value(hwnd, L"Порог (мс)", g_thr, 1, 1000, &v)) {
                g_thr = v;
                apply_change();
            }
            break;
        }
        case ID_POST_CUSTOM: {
            UINT64 v;
            if (input_value(hwnd, L"Пост-задержка (мс)", g_post, 0, 1000, &v)) {
                g_post = v;
                apply_change();
            }
            break;
        }
        case ID_AUTO:
            toggle_autostart(hwnd);
            break;
        case ID_EXIT:
            DestroyWindow(hwnd);
            break;
        default: {
            UINT id = LOWORD(wp);
            if (id >= ID_THR_BASE && id < ID_THR_BASE + (UINT)THR_COUNT) {
                g_thr = THRESHOLDS[id - ID_THR_BASE];
                apply_change();
            }
            else if (id >= ID_POST_BASE && id < ID_POST_BASE + (UINT)POST_COUNT) {
                g_post = POST_DELAYS[id - ID_POST_BASE];
                apply_change();
            }
            else if (id >= ID_BTN_BASE && id < ID_BTN_BASE + (UINT)BTN_COUNT) {
                int i = (int)(id - ID_BTN_BASE);
                g_btn_on[i] = !g_btn_on[i];
                apply_change();
            }
            break;
        }
        }
        return 0;

    case WM_ENDSESSION:
        if (wp) remove_tray();
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        remove_tray();
        if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
        if (g_mutex) { CloseHandle(g_mutex); g_mutex = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    g_mutex = CreateMutexW(NULL, FALSE, L"Local\\AntiDoubleClickSingleton");
    if (!g_mutex) {
        MessageBoxW(NULL, L"Не удалось создать мьютекс", L"Anti-Double-Click", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Anti-Double-Click уже запущен", L"Anti-Double-Click", MB_OK | MB_ICONINFORMATION);
        CloseHandle(g_mutex);
        return 0;
    }

    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpc_freq = (double)f.QuadPart / 1000000.0; }
    load_settings();

    /* Регистрируется до создания окна, чтобы не пропустить сообщение. */
    g_msg_taskbar = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"AntiDCWnd";
    if (!RegisterClassExW(&wc)) { CloseHandle(g_mutex); return 1; }

    /* Окно намеренно обычное, а не HWND_MESSAGE: message-only окна не получают
       широковещательное TaskbarCreated. */
    g_hwnd = CreateWindowExW(0, L"AntiDCWnd", L"AntiDC", 0,
        0, 0, 0, 0, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_hwnd) { CloseHandle(g_mutex); return 1; }

    try_add_tray();

    if (!install_hook()) {
        MessageBoxW(NULL, L"Не удалось установить хук", L"Anti-Double-Click", MB_OK | MB_ICONERROR);
        remove_tray();
        DestroyWindow(g_hwnd);
        if (g_mutex) { CloseHandle(g_mutex); g_mutex = NULL; }
        return 1;
    }

    GetCursorPos(&g_last_pt);
    SetTimer(g_hwnd, TIMER_ID, TIMER_MS, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
