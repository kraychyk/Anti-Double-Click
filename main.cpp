#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <wchar.h>

#define WM_TRAY (WM_USER + 1)
#define TRAY_ID 1

#define ID_TOGGLE      100
#define ID_THR_BASE    200
#define ID_THR_CUSTOM  501
#define ID_POST_BASE   600
#define ID_POST_CUSTOM 801
#define ID_AUTO        901
#define ID_EXIT        902
#define IDI_APP        101
#define HOLD_MS        30u
#define IDD_INPUT      200
#define IDC_EDIT       1001
#define IDC_TEXT       1002

static const wchar_t* RUN_KEY   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_NAME  = L"AntiDoubleClick";
static const wchar_t* SETUP_KEY = L"Software\\AntiDoubleClick";

static const UINT64 THRESHOLDS[] = {5,10,20,30,50,80,100,200,300};
static const int THR_COUNT = sizeof(THRESHOLDS)/sizeof(THRESHOLDS[0]);

static const UINT64 POST_DELAYS[] = {0,15,20,30,40,50,80,100,200};
static const int POST_COUNT = sizeof(POST_DELAYS)/sizeof(POST_DELAYS[0]);

static BOOL    g_on      = TRUE;
static UINT64  g_thr     = 5;
static UINT64  g_post    = 0;
static UINT64  g_blocked = 0;
static UINT64  g_last_down = 0;
static UINT64  g_last_up   = 0;
static UINT64  g_ignore_until = 0;
static HHOOK   g_hook    = NULL;
static HWND    g_hwnd    = NULL;
static HANDLE  g_mutex   = NULL;
static double g_qpc_freq = 1.0;

static UINT64 now_us() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (UINT64)(li.QuadPart / g_qpc_freq);
}

static void load_settings() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETUP_KEY, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD val, sz = sizeof(DWORD);
        if (RegQueryValueExW(key, L"Threshold", 0, 0, (BYTE*)&val, &sz) == ERROR_SUCCESS) {
            if (val >= 1 && val <= 1000) { g_thr = val; }
        }
        sz = sizeof(DWORD);
        if (RegQueryValueExW(key, L"PostDelay", 0, 0, (BYTE*)&val, &sz) == ERROR_SUCCESS) {
            if (val <= 1000) { g_post = val; }
        }
        sz = sizeof(DWORD);
        if (RegQueryValueExW(key, L"Enabled", 0, 0, (BYTE*)&val, &sz) == ERROR_SUCCESS) {
            g_on = (val != 0);
        }
        RegCloseKey(key);
    }
}

static void save_settings() {
    HKEY key;
    if (RegCreateKeyW(HKEY_CURRENT_USER, SETUP_KEY, &key) == ERROR_SUCCESS) {
        DWORD v = (DWORD)g_thr;
        RegSetValueExW(key, L"Threshold", 0, REG_DWORD, (BYTE*)&v, sizeof(DWORD));
        v = (DWORD)g_post;
        RegSetValueExW(key, L"PostDelay", 0, REG_DWORD, (BYTE*)&v, sizeof(DWORD));
        v = g_on ? 1 : 0;
        RegSetValueExW(key, L"Enabled", 0, REG_DWORD, (BYTE*)&v, sizeof(DWORD));
        RegCloseKey(key);
    }
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

static void toggle_autostart() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        if (is_autostart()) {
            RegDeleteValueW(key, RUN_NAME);
        } else {
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(NULL, path, MAX_PATH);
            RegSetValueExW(key, RUN_NAME, 0, REG_SZ, (BYTE*)path, (DWORD)((wcslen(path)+1)*2));
        }
        RegCloseKey(key);
    }
}

static LRESULT CALLBACK mouse_proc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && g_on) {
        if (wp == WM_LBUTTONDOWN) {
            UINT64 now = now_us();

            if (g_post > 0 && now < g_ignore_until) {
                g_blocked++;
                g_last_down = now;
                return 1;
            }

            if (g_last_down > 0 && (now - g_last_down) < HOLD_MS * 1000) {
                g_blocked++;
                g_last_down = now;
                return 1;
            }

            if (g_last_up > 0 && (now - g_last_up) < g_thr * 1000) {
                g_blocked++;
                g_last_down = now;
                return 1;
            }

            g_last_down = now;
        }
        if (wp == WM_LBUTTONUP) {
            UINT64 now = now_us();
            g_last_up = now;
            g_ignore_until = now + g_post * 1000;
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

static void fill_tip(NOTIFYICONDATAW* nid, const wchar_t* tip) {
    wcsncpy_s(nid->szTip, tip, _TRUNCATE);
}

static void refresh_tip() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_TIP;
    const wchar_t* st = g_on ? L"\x0412\x043A\x043B" : L"\x0412\x044B\x043A\x043B";
    wchar_t buf[256];
    swprintf_s(buf, L"ADC | %s | %llu/%llu\x043C\x0441 | \x0411\x043B\x043E\x043A: %llu", st, g_thr, g_post, g_blocked);
    fill_tip(&nid, buf);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void add_tray() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCE(IDI_APP));
    if (!nid.hIcon) nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    fill_tip(&nid, L"Anti-Double-Click");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void remove_tray() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
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
        SetDlgItemTextW(hdlg, IDC_TEXT, L"\x0417\x043D\x0430\x0447\x0435\x043D\x0438\x0435 \x0432 \x043C\x0441:");
        SetDlgItemTextW(hdlg, IDCANCEL, L"\x041E\x0442\x043C\x0435\x043D\x0430");
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
                swprintf_s(err, L"\x0412\x0432\x0435\x0434\x0438\x0442\x0435 \x0447\x0438\x0441\x043B\x043E \x043E\x0442 %llu \x0434\x043E %llu", ctx->min, ctx->max);
                MessageBoxW(hdlg, err, L"\x041E\x0448\x0438\x0431\x043A\x0430", MB_OK | MB_ICONWARNING);
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
    INT_PTR r = DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCE(IDD_INPUT), parent, input_dlg_proc, (LPARAM)&ctx);
    if (r == IDOK && ctx.ok) { *out = ctx.result; return TRUE; }
    return FALSE;
}

static void show_menu() {
    HMENU menu = CreatePopupMenu();

    const wchar_t* tog = g_on ? L"\x0412\x044B\x043A\x043B\x044E\x0447\x0438\x0442\x044C" : L"\x0412\x043A\x043B\x044E\x0447\x0438\x0442\x044C";
    AppendMenuW(menu, MF_STRING, ID_TOGGLE, tog);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    HMENU sub = CreatePopupMenu();
    for (int i = 0; i < THR_COUNT; i++) {
        wchar_t label[32];
        swprintf_s(label, L"%llu \x043C\x0441", THRESHOLDS[i]);
        UINT id = ID_THR_BASE + (UINT)THRESHOLDS[i];
        AppendMenuW(sub, MF_STRING, id, label);
        if (g_thr == THRESHOLDS[i])
            CheckMenuItem(sub, id, MF_CHECKED);
    }
    AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sub, MF_STRING, ID_THR_CUSTOM, L"\x0421\x0432\x043E\x0435 \x0437\x043D\x0430\x0447\x0435\x043D\x0438\x0435...");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"\x041F\x043E\x0440\x043E\x0433");

    HMENU sub2 = CreatePopupMenu();
    for (int i = 0; i < POST_COUNT; i++) {
        wchar_t label[32];
        if (POST_DELAYS[i] == 0)
            swprintf_s(label, L"\x0412\x044B\x043A\x043B");
        else
            swprintf_s(label, L"%llu \x043C\x0441", POST_DELAYS[i]);
        UINT id = ID_POST_BASE + (UINT)POST_DELAYS[i];
        AppendMenuW(sub2, MF_STRING, id, label);
        if (g_post == POST_DELAYS[i])
            CheckMenuItem(sub2, id, MF_CHECKED);
    }
    AppendMenuW(sub2, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sub2, MF_STRING, ID_POST_CUSTOM, L"\x0421\x0432\x043E\x0435 \x0437\x043D\x0430\x0447\x0435\x043D\x0438\x0435...");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub2, L"\x041F\x043E\x0441\x0442-\x0437\x0430\x0434\x0435\x0440\x0436\x043A\x0430");

    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    UINT af = is_autostart() ? MF_CHECKED : 0;
    AppendMenuW(menu, MF_STRING | af, ID_AUTO, L"\x0410\x0432\x0442\x043E\x0437\x0430\x043F\x0443\x0441\x043A");
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"\x0412\x044B\x0445\x043E\x0434");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    PostMessage(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(sub2);
    DestroyMenu(sub);
    DestroyMenu(menu);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if (lp == WM_LBUTTONUP || lp == WM_RBUTTONUP) { show_menu(); }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_TOGGLE:
            g_on = !g_on;
            if (!g_on) { g_last_down = 0; g_last_up = 0; g_ignore_until = 0; }
            save_settings();
            refresh_tip();
            break;
        case ID_THR_CUSTOM: {
            UINT64 v;
            if (input_value(hwnd, L"\x041F\x043E\x0440\x043E\x0433 (\x043C\x0441)", g_thr, 1, 1000, &v)) {
                g_thr = v;
                g_last_down = 0; g_last_up = 0; g_ignore_until = 0;
                save_settings();
                refresh_tip();
            }
            break;
        }
        case ID_POST_CUSTOM: {
            UINT64 v;
            if (input_value(hwnd, L"\x041F\x043E\x0441\x0442-\x0437\x0430\x0434\x0435\x0440\x0436\x043A\x0430 (\x043C\x0441)", g_post, 0, 1000, &v)) {
                g_post = v;
                g_last_down = 0; g_last_up = 0; g_ignore_until = 0;
                save_settings();
                refresh_tip();
            }
            break;
        }
        case ID_AUTO:
            toggle_autostart();
            break;
        case ID_EXIT:
            remove_tray();
            if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
            if (g_mutex) { CloseHandle(g_mutex); g_mutex = NULL; }
            PostQuitMessage(0);
            break;
        default: {
            UINT id = LOWORD(wp);
            if (id >= ID_THR_BASE && id < ID_POST_BASE) {
                g_thr = id - ID_THR_BASE;
                g_last_down = 0;
                g_last_up = 0;
                g_ignore_until = 0;
                save_settings();
                refresh_tip();
            }
            else if (id >= ID_POST_BASE && id < ID_AUTO) {
                g_post = id - ID_POST_BASE;
                g_last_down = 0;
                g_last_up = 0;
                g_ignore_until = 0;
                save_settings();
                refresh_tip();
            }
            break;
        }
        }
        return 0;
    case WM_DESTROY:
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
    if (!g_mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(NULL, L"Anti-Double-Click \x0443\x0436\x0435 \x0437\x0430\x043F\x0443\x0449\x0435\x043D", L"Anti-Double-Click", MB_OK | MB_ICONINFORMATION);
        if (g_mutex) CloseHandle(g_mutex);
        return 0;
    }

    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpc_freq = (double)f.QuadPart / 1000000.0; }
    load_settings();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"AntiDCWnd";
    if (!RegisterClassExW(&wc)) return 1;

    g_hwnd = CreateWindowExW(0, L"AntiDCWnd", L"AntiDC", 0,
        0, 0, 0, 0, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!g_hwnd) return 1;

    add_tray();
    refresh_tip();

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, NULL, 0);
    if (!g_hook) {
        MessageBoxW(NULL, L"\x041D\x0435 \x0443\x0434\x0430\x043B\x043E\x0441\x044C \x0443\x0441\x0442\x0430\x043D\x043E\x0432\x0438\x0442\x044C \x0445\x0443\x043A", L"Anti-Double-Click", MB_OK);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}