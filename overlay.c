/*
 * overlay.c - always-on-top hardware overlay + tray icon for Windows.
 *
 * Sensors, read straight from the source (no helper programs):
 *   CPU load / RAM     GetSystemTimes, GlobalMemoryStatusEx
 *   Temps / fan RPM    ASUS BIOS WMI interface (root\WMI ASUSHW) - needs admin
 *   GPU                NVIDIA NVML (nvml.dll ships with the driver)
 *   Claude sessions    state files written by claude-state.ps1 hooks
 *
 * Phone view: the same stats are served as a web page on the local network,
 * http://<this PC>:8787 (read-only; the address is shown in the tray menu).
 *
 * Build:  build.bat   (embeds a "run as administrator" manifest)
 *
 * Hotkeys:  Ctrl+Alt+O  show/hide overlay
 *           Ctrl+Alt+Q  quit
 * Tray icon: shows CPU load %; hover for all stats, left-click to
 *            show/hide the overlay, right-click for a menu that also lists
 *            the Claude sessions and whether each is busy or idle.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <wbemidl.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#define WIN_W        360
#define LINE_H       26
#define PAD          8
#define MARGIN       10
#define UPDATE_MS    1000
#define MAX_SENS     16
#define HK_TOGGLE    1
#define HK_QUIT      2
#define WM_APP_DATA  (WM_APP + 1)
#define WM_APP_TRAY  (WM_APP + 2)
#define IDM_TOGGLE   100
#define IDM_QUIT     101
#define IDM_SESSION  102                /* session rows; clicking does nothing */
#define WEB_PORT     8787

enum { KIND_TEMP = 1, KIND_FAN = 2 };   /* ASUSHW sensor "Type" values */

typedef struct {
    wchar_t label[32];
    int kind;
    int value;
} Sensor;

#define MAX_CLAUDE   16

typedef struct {
    wchar_t sid[40];
    wchar_t label[128];                 /* "folder: latest prompt" */
    int state;                          /* CS_IDLE, CS_BG or CS_BUSY */
    int agents;                         /* subagents + background tasks */
} ClaudeSession;

enum { CS_IDLE, CS_BG, CS_BUSY };      /* CS_BG: turn done, background work running */
static const wchar_t *const CS_NAME[] = { L"idle", L"bg", L"busy" };

typedef struct {
    double cpu;
    double mem_used_gb, mem_total_gb;
    DWORD mem_load;
    int cpu_temp;                       /* -1 = unknown */
    int gpu_ok;
    unsigned gpu_temp, gpu_load, gpu_fan;
    int nsens;
    Sensor sens[MAX_SENS];
    int claude_ok;                      /* hook state folder exists */
    int claude_busy, claude_bg, claude_idle, claude_agents;
    int nclaude;
    ClaudeSession claude[MAX_CLAUDE];
} Snapshot;

static Snapshot g_snap;                 /* guarded by g_lock */
static CRITICAL_SECTION g_lock;
static HANDLE g_stop;
static HWND g_hwnd;
static HFONT g_font;
static HICON g_tray_icon;
static UINT g_taskbar_created;
static int g_win_h;

/* ---------------------------------------------------------------- CPU/RAM */

static ULONGLONG ft_to_u64(FILETIME ft)
{
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

/* Total CPU usage since the previous call, in percent. */
static double sample_cpu(void)
{
    static ULONGLONG prev_idle, prev_kernel, prev_user;
    FILETIME idle, kernel, user;
    ULONGLONG i, k, u, d_idle, d_total;
    double pct = 0.0;

    if (!GetSystemTimes(&idle, &kernel, &user))
        return 0.0;
    i = ft_to_u64(idle);
    k = ft_to_u64(kernel);   /* kernel time includes idle time */
    u = ft_to_u64(user);

    d_idle  = i - prev_idle;
    d_total = (k - prev_kernel) + (u - prev_user);
    if (prev_kernel && d_total)
        pct = 100.0 * (double)(d_total - d_idle) / (double)d_total;

    prev_idle = i; prev_kernel = k; prev_user = u;
    return pct;
}

/* ---------------------------------------------------- ASUS BIOS sensors */

static IWbemServices *g_svc;
static IWbemClassObject *g_asus_cls;
static BSTR g_asus_path;
static int g_asus_idx[MAX_SENS];        /* ASUSHW index of each shown sensor */
static int g_asus_src[4];               /* distinct sensor sources to refresh */
static int g_asus_nsrc;

/* Call an ASUSHW method with an optional uint8 argument. */
static IWbemClassObject *asus_call(const wchar_t *method, const wchar_t *arg, int value)
{
    IWbemClassObject *sig = NULL, *in = NULL, *out = NULL;
    BSTR name = SysAllocString(method);

    if (arg && SUCCEEDED(IWbemClassObject_GetMethod(g_asus_cls, method, 0, &sig, NULL)) && sig) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI1;
        v.bVal = (BYTE)value;
        if (SUCCEEDED(IWbemClassObject_SpawnInstance(sig, 0, &in)))
            IWbemClassObject_Put(in, arg, 0, &v, 0);
    }
    if (FAILED(IWbemServices_ExecMethod(g_svc, g_asus_path, name, 0, NULL, in, &out, NULL)))
        out = NULL;

    if (in) IWbemClassObject_Release(in);
    if (sig) IWbemClassObject_Release(sig);
    SysFreeString(name);
    return out;
}

static int out_int(IWbemClassObject *out, const wchar_t *prop)
{
    VARIANT v;
    int r = -1;
    if (out && SUCCEEDED(IWbemClassObject_Get(out, prop, 0, &v, NULL, NULL))) {
        if (v.vt == VT_I4) r = v.lVal;
        else if (v.vt == VT_UI1) r = v.bVal;
        VariantClear(&v);
    }
    return r;
}

static int asus_value(int index)
{
    IWbemClassObject *out = asus_call(L"sensor_get_value", L"Index", index);
    int r = out_int(out, L"Data");
    if (out) IWbemClassObject_Release(out);
    return r;
}

static void asus_refresh(int source)
{
    IWbemClassObject *out = asus_call(L"sensor_update_buffer", L"Source", source);
    if (out) IWbemClassObject_Release(out);
}

static void add_source(int src)
{
    int i;
    for (i = 0; i < g_asus_nsrc; i++)
        if (g_asus_src[i] == src) return;
    if (g_asus_nsrc < 4) g_asus_src[g_asus_nsrc++] = src;
}

/* Connect to ASUSHW and pick the temperatures and fans that read sensibly
   right now (skips unplugged fan headers and absent probes). */
static int asus_init(Snapshot *s)
{
    IWbemLocator *loc = NULL;
    IEnumWbemClassObject *en = NULL;
    IWbemClassObject *obj = NULL, *out;
    BSTR ns = SysAllocString(L"ROOT\\WMI"), cls = SysAllocString(L"ASUSHW");
    ULONG got = 0;
    VARIANT v;
    int count, i, ok = 0;

    if (FAILED(CoCreateInstance(&CLSID_WbemLocator, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWbemLocator, (void **)&loc)))
        goto done;
    if (FAILED(IWbemLocator_ConnectServer(loc, ns, NULL, NULL, NULL, 0, NULL, NULL, &g_svc)))
        goto done;
    CoSetProxyBlanket((IUnknown *)g_svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    if (FAILED(IWbemServices_GetObject(g_svc, cls, 0, NULL, &g_asus_cls, NULL)))
        goto done;
    if (FAILED(IWbemServices_CreateInstanceEnum(g_svc, cls,
               WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en)))
        goto done;
    if (IEnumWbemClassObject_Next(en, WBEM_INFINITE, 1, &obj, &got) != S_OK || !got)
        goto done;           /* access denied ends up here when not admin */
    if (FAILED(IWbemClassObject_Get(obj, L"__PATH", 0, &v, NULL, NULL)))
        goto done;
    g_asus_path = SysAllocString(v.bstrVal);
    VariantClear(&v);

    out = asus_call(L"sensor_get_number", NULL, 0);
    count = out_int(out, L"Data");
    if (out) IWbemClassObject_Release(out);

    for (i = 0; i < count && s->nsens < MAX_SENS; i++) {
        int type, src, val;
        wchar_t *suffix;
        Sensor *se;

        out = asus_call(L"sensor_get_info", L"Index", i);
        if (!out) continue;
        type = out_int(out, L"Type");
        src  = out_int(out, L"Source");
        if (type != KIND_TEMP && type != KIND_FAN) {
            IWbemClassObject_Release(out);
            continue;
        }
        asus_refresh(src);
        val = asus_value(i);
        if ((type == KIND_TEMP && (val <= 0 || val > 125)) ||
            (type == KIND_FAN && val <= 0)) {
            IWbemClassObject_Release(out);
            continue;
        }

        se = &s->sens[s->nsens];
        se->kind = type;
        se->value = val;
        se->label[0] = 0;
        if (SUCCEEDED(IWbemClassObject_Get(out, L"Name", 0, &v, NULL, NULL))) {
            if (v.vt == VT_BSTR)
                wcsncpy_s(se->label, 32, v.bstrVal, _TRUNCATE);
            VariantClear(&v);
        }
        /* "Motherboard Temperature" -> "Motherboard temp" */
        suffix = wcsstr(se->label, L" Temperature");
        if (suffix) wcscpy_s(suffix, 32 - (suffix - se->label), L" temp");

        g_asus_idx[s->nsens++] = i;
        add_source(src);
        IWbemClassObject_Release(out);
        ok = 1;
    }

done:
    if (obj) IWbemClassObject_Release(obj);
    if (en) IEnumWbemClassObject_Release(en);
    if (loc) IWbemLocator_Release(loc);
    SysFreeString(ns);
    SysFreeString(cls);
    return ok;
}

static void asus_sample(Snapshot *s)
{
    int i;
    for (i = 0; i < g_asus_nsrc; i++)
        asus_refresh(g_asus_src[i]);
    for (i = 0; i < s->nsens; i++) {
        int v = asus_value(g_asus_idx[i]);
        if (v >= 0) s->sens[i].value = v;
    }
}

/* ------------------------------------------------------------ NVIDIA GPU */

typedef struct { unsigned gpu, memory; } NvmlUtil;
typedef int (*nvml_init_t)(void);
typedef int (*nvml_handle_t)(unsigned, void **);
typedef int (*nvml_temp_t)(void *, int, unsigned *);
typedef int (*nvml_uint_t)(void *, unsigned *);
typedef int (*nvml_util_t)(void *, NvmlUtil *);

static void *g_gpu;
static nvml_temp_t nvml_temp;
static nvml_uint_t nvml_fan;
static nvml_util_t nvml_util;

static int gpu_init(void)
{
    HMODULE dll = LoadLibraryExW(L"nvml.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    nvml_init_t init;
    nvml_handle_t handle;

    if (!dll) return 0;
    init      = (nvml_init_t)GetProcAddress(dll, "nvmlInit_v2");
    handle    = (nvml_handle_t)GetProcAddress(dll, "nvmlDeviceGetHandleByIndex_v2");
    nvml_temp = (nvml_temp_t)GetProcAddress(dll, "nvmlDeviceGetTemperature");
    nvml_fan  = (nvml_uint_t)GetProcAddress(dll, "nvmlDeviceGetFanSpeed");
    nvml_util = (nvml_util_t)GetProcAddress(dll, "nvmlDeviceGetUtilizationRates");
    if (!init || !handle || !nvml_temp || !nvml_fan || !nvml_util)
        return 0;
    return init() == 0 && handle(0, &g_gpu) == 0;
}

static void gpu_sample(Snapshot *s)
{
    NvmlUtil u = {0};
    nvml_temp(g_gpu, 0, &s->gpu_temp);      /* 0 = NVML_TEMPERATURE_GPU */
    nvml_fan(g_gpu, &s->gpu_fan);
    nvml_util(g_gpu, &u);
    s->gpu_load = u.gpu;
}

/* ------------------------------------------------- Claude Code sessions */

/* claude-state.ps1, run from Claude Code hooks, keeps one file per session
   (lines: "working"/"idle", project folder, first prompt) and one per running
   subagent or background task under this folder. Files that stop being
   updated (crashed or interrupted sessions) expire. */
static wchar_t g_claude_dir[MAX_PATH];

#define BUSY_EXPIRE_S   (2 * 3600)
#define IDLE_EXPIRE_S   (24 * 3600)

static ULONGLONG file_age_s(const WIN32_FIND_DATAW *fd)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return (ft_to_u64(now) - ft_to_u64(fd->ftLastWriteTime)) / 10000000ULL;
}

/* Read a small UTF-8 text file into wide lines (up to 3). */
static int read_lines(const wchar_t *path, wchar_t lines[3][128])
{
    char buf[512];
    wchar_t wide[512], *p = wide, *e, c;
    DWORD got = 0;
    int n = 0, len;
    HANDLE f = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);

    lines[0][0] = lines[1][0] = lines[2][0] = 0;
    if (f == INVALID_HANDLE_VALUE) return 0;
    ReadFile(f, buf, sizeof buf - 1, &got, NULL);
    CloseHandle(f);
    len = MultiByteToWideChar(CP_UTF8, 0, buf, (int)got, wide, 511);
    wide[len] = 0;

    /* Split on CRLF or LF, keeping empty lines in place. */
    while (n < 3) {
        e = wcspbrk(p, L"\r\n");
        c = e ? *e : 0;
        if (e) *e = 0;
        wcsncpy_s(lines[n++], 128, p, _TRUNCATE);
        if (!e) break;
        p = e + 1;
        if (c == L'\r' && *p == L'\n') p++;
    }
    return n;
}

static void claude_sample(Snapshot *s)
{
    wchar_t pat[MAX_PATH], path[MAX_PATH], lines[3][128];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int i;

    s->claude_busy = s->claude_bg = s->claude_idle = s->claude_agents = 0;
    s->nclaude = 0;

    swprintf(pat, MAX_PATH, L"%s\\sessions\\*", g_claude_dir);
    h = FindFirstFileW(pat, &fd);
    s->claude_ok = h != INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            int busy;
            ClaudeSession *cs;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            swprintf(path, MAX_PATH, L"%s\\sessions\\%s", g_claude_dir, fd.cFileName);
            read_lines(path, lines);
            if (lines[2][0] == L'<') lines[2][0] = 0;      /* system message, not a prompt */
            busy = wcscmp(lines[0], L"working") == 0;
            if (busy) {
                if (file_age_s(&fd) >= BUSY_EXPIRE_S) continue;
                s->claude_busy++;
            } else {
                if (file_age_s(&fd) >= IDLE_EXPIRE_S) continue;
                s->claude_idle++;
            }
            if (s->nclaude >= MAX_CLAUDE) continue;
            cs = &s->claude[s->nclaude++];
            cs->state = busy ? CS_BUSY : CS_IDLE;
            cs->agents = 0;
            wcsncpy_s(cs->sid, 40, fd.cFileName, _TRUNCATE);
            if (lines[1][0] && lines[2][0])
                swprintf(cs->label, 128, L"%s: %s", lines[1], lines[2]);
            else if (lines[1][0] || lines[2][0])
                wcsncpy_s(cs->label, 128, lines[1][0] ? lines[1] : lines[2], _TRUNCATE);
            else
                swprintf(cs->label, 128, L"session %.8s", fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    /* Agent files are named "<session id>.<agent id>" or "<session id>.bg-<tool call id>". */
    swprintf(pat, MAX_PATH, L"%s\\agents\\*", g_claude_dir);
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                file_age_s(&fd) >= BUSY_EXPIRE_S)
                continue;
            s->claude_agents++;
            for (i = 0; i < s->nclaude; i++) {
                size_t n = wcslen(s->claude[i].sid);
                if (wcsncmp(fd.cFileName, s->claude[i].sid, n) == 0 && fd.cFileName[n] == L'.')
                    s->claude[i].agents++;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    /* An idle session with work still running in the background isn't idle. */
    for (i = 0; i < s->nclaude; i++) {
        if (s->claude[i].state == CS_IDLE && s->claude[i].agents) {
            s->claude[i].state = CS_BG;
            s->claude_idle--;
            s->claude_bg++;
        }
    }
}

/* ------------------------------------------------------- sampler thread */

/* WMI calls into the BIOS can take tens of milliseconds, so sampling runs
   on its own thread and posts finished snapshots to the window. */
static DWORD WINAPI sampler(LPVOID unused)
{
    Snapshot s = {0};
    MEMORYSTATUSEX mem;
    int have_asus, i;
    (void)unused;

    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                         RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    have_asus = asus_init(&s);
    s.gpu_ok = gpu_init();
    sample_cpu();                       /* prime the CPU counters */

    do {
        s.cpu = sample_cpu();
        mem.dwLength = sizeof mem;
        GlobalMemoryStatusEx(&mem);
        s.mem_load = mem.dwMemoryLoad;
        s.mem_total_gb = mem.ullTotalPhys / (1024.0 * 1024 * 1024);
        s.mem_used_gb = (mem.ullTotalPhys - mem.ullAvailPhys) / (1024.0 * 1024 * 1024);
        if (have_asus) asus_sample(&s);
        if (s.gpu_ok) gpu_sample(&s);
        claude_sample(&s);

        s.cpu_temp = -1;
        for (i = 0; i < s.nsens; i++)
            if (s.sens[i].kind == KIND_TEMP && wcscmp(s.sens[i].label, L"CPU temp") == 0)
                s.cpu_temp = s.sens[i].value;

        EnterCriticalSection(&g_lock);
        g_snap = s;
        LeaveCriticalSection(&g_lock);
        PostMessageW(g_hwnd, WM_APP_DATA, 0, 0);
    } while (WaitForSingleObject(g_stop, UPDATE_MS) == WAIT_TIMEOUT);

    CoUninitialize();
    return 0;
}

/* ------------------------------------------------------------ phone view */

static char g_web_url[64];              /* "http://192.168.x.x:8787", or empty */

static const char WEB_PAGE[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>hw-overlay</title><style>"
    "body{margin:0;background:#111;color:#ddd;font:16px/1.4 ui-monospace,Consolas,monospace;padding:12px}"
    ".r{display:flex;justify-content:space-between;padding:6px 4px;border-bottom:1px solid #222}"
    ".r span:first-child{overflow:hidden;text-overflow:ellipsis;white-space:nowrap;margin-right:12px}"
    ".v{font-weight:bold;white-space:nowrap}.h{color:#888;font-size:13px;margin:16px 4px 2px}#e{color:#f55;padding:8px 4px}"
    "#k,#s{width:100%;margin:0 0 8px;padding:10px;background:#222;color:#ddd;border:1px solid #333;border-radius:6px;font:inherit}"
    "#k.on{color:#50e678}#s{margin-top:16px;color:#888}"
    "</style></head><body><button id=k hidden>Keep screen on</button><div id=m></div><div id=e></div><button id=s hidden>Change PC address</button><script>"
    /* Keep the phone awake with the Wake Lock API. Chrome only offers it on
       secure origins, so the button only shows where it can work: over HTTPS,
       on localhost, or with this address allowed in chrome://flags. The choice
       is remembered on the phone, and the lock is taken again whenever Chrome
       drops it (tab hidden, page reloaded) while the page is visible.
       Turning it on also goes full screen (that part needs the tap itself).
       Inside the Android app (window.HwApp) the app does both instead. */
    "let lock=null,want=false;try{want=localStorage.getItem('awake')=='1'}catch(x){}"
    "function label(){k.textContent=want?(lock?'Screen stays on':'Keep screen on (waiting)'):'Keep screen on';"
    "k.classList.toggle('on',want&&!!lock)}"
    "async function awake(){if(!want||lock||document.visibilityState!='visible')return;"
    "try{lock=await navigator.wakeLock.request('screen');"
    "lock.onrelease=()=>{lock=null;label();setTimeout(awake,500)}}catch(x){}label()}"
    "k.onclick=()=>{want=!want;try{localStorage.setItem('awake',want?'1':'0')}catch(x){}"
    "if(want){awake();document.documentElement.requestFullscreen&&document.documentElement.requestFullscreen().catch(()=>{})}"
    "else{if(lock){const l=lock;lock=null;l.release()}document.fullscreenElement&&document.exitFullscreen()}label()};"
    "if(window.HwApp){const a=window.HwApp;want=a.isAwake();k.hidden=s.hidden=false;"
    "const lb=()=>{k.textContent=want?'Screen stays on':'Keep screen on';k.classList.toggle('on',want)};lb();"
    "k.onclick=()=>{want=!want;a.setAwake(want);lb()};s.onclick=()=>a.changeAddress()}"
    "else if(navigator.wakeLock){k.hidden=false;label();awake();"
    "document.addEventListener('visibilitychange',awake);setInterval(awake,5000)}"
    "const G='#50e678',Y='#f0d23c',R='#ff5050',F='#78c8ff',C='#e68c5a',I='#969696';"
    "const load=p=>p<60?G:p<85?Y:R,temp=c=>c<60?G:c<80?Y:R;"
    "const esc=s=>s.replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));"
    "const row=(l,v,c)=>`<div class=r><span>${l}</span><span class=v style='color:${c}'>${v}</span></div>`;"
    "async function tick(){try{"
    "const d=await(await fetch('data.json',{cache:'no-store'})).json();"
    "let h=row('CPU',d.cpu.toFixed(1)+' %',load(d.cpu))"
    "+row('RAM',d.ram_used.toFixed(1)+' / '+d.ram_total.toFixed(1)+' GB',load(d.ram_load));"
    "if(d.gpu)h+=row('GPU',d.gpu.load+'%  '+d.gpu.temp+'\xC2\xB0\x43  fan '+d.gpu.fan+'%',temp(d.gpu.temp));"
    "for(const s of d.sensors)h+=s.kind=='temp'"
    "?row(esc(s.label),s.value+'\xC2\xB0\x43',temp(s.value)):row(esc(s.label),s.value+' rpm',F);"
    "if(d.claude){const c=d.claude;"
    "h+='<div class=h>Claude</div>'+row('Sessions',c.busy+' busy / '+c.bg+' bg / '+c.idle+' idle',c.busy?C:I);"
    "for(const s of c.sessions)h+=row(esc(s.label),s.state+(s.agents?' +'+s.agents:''),"
    "s.state=='busy'?C:s.state=='bg'?Y:I);"
    "h+=row('Background',c.background,c.background?C:I);}"
    "m.innerHTML=h;e.textContent=''}catch(x){e.textContent='PC not reachable'}}"
    "tick();setInterval(tick,1000)"
    "</script></body></html>";

typedef struct { char d[16384]; size_t n; } Buf;

static void bprintf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = vsnprintf(b->d + b->n, sizeof b->d - b->n, fmt, ap);
    va_end(ap);
    if (r > 0) b->n += (size_t)r < sizeof b->d - b->n ? (size_t)r : sizeof b->d - b->n - 1;
}

/* Append a wide string as a JSON string literal (UTF-8, escaped). */
static void bjson_str(Buf *b, const wchar_t *w)
{
    char utf8[512];
    const unsigned char *p;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8, sizeof utf8, NULL, NULL);
    bprintf(b, "\"");
    for (p = (const unsigned char *)utf8; *p; p++) {
        if (*p == '"' || *p == '\\') bprintf(b, "\\%c", *p);
        else if (*p < 0x20) bprintf(b, "\\u%04x", *p);
        else bprintf(b, "%c", *p);
    }
    bprintf(b, "\"");
}

static void build_json(Buf *b)
{
    Snapshot s;
    int i, st, first;

    EnterCriticalSection(&g_lock);
    s = g_snap;
    LeaveCriticalSection(&g_lock);

    b->n = 0;
    bprintf(b, "{\"cpu\":%.1f,\"ram_used\":%.2f,\"ram_total\":%.2f,\"ram_load\":%lu,",
            s.cpu, s.mem_used_gb, s.mem_total_gb, s.mem_load);
    if (s.gpu_ok)
        bprintf(b, "\"gpu\":{\"load\":%u,\"temp\":%u,\"fan\":%u},", s.gpu_load, s.gpu_temp, s.gpu_fan);
    else
        bprintf(b, "\"gpu\":null,");

    bprintf(b, "\"sensors\":[");
    for (i = 0; i < s.nsens; i++) {
        bprintf(b, "%s{\"label\":", i ? "," : "");
        bjson_str(b, s.sens[i].label);
        bprintf(b, ",\"kind\":\"%s\",\"value\":%d}",
                s.sens[i].kind == KIND_TEMP ? "temp" : "fan", s.sens[i].value);
    }
    bprintf(b, "],");

    if (!s.claude_ok) {
        bprintf(b, "\"claude\":null}");
        return;
    }
    bprintf(b, "\"claude\":{\"busy\":%d,\"bg\":%d,\"idle\":%d,\"background\":%d,\"sessions\":[",
            s.claude_busy, s.claude_bg, s.claude_idle, s.claude_agents);
    first = 1;
    for (st = CS_BUSY; st >= CS_IDLE; st--) {
        for (i = 0; i < s.nclaude; i++) {
            if (s.claude[i].state != st) continue;
            bprintf(b, "%s{\"label\":", first ? "" : ",");
            bjson_str(b, s.claude[i].label);
            bprintf(b, ",\"state\":\"%ls\",\"agents\":%d}", CS_NAME[st], s.claude[i].agents);
            first = 0;
        }
    }
    bprintf(b, "]}}");
}

static void web_send(SOCKET c, const char *status, const char *type, const char *body, size_t len)
{
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                     "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                     status, type, len);
    send(c, head, n, 0);
    if (len) send(c, body, (int)len, 0);
}

/* Find this PC's address on the local network, for the tray menu. */
static void find_lan_url(void)
{
    char host[256];
    struct addrinfo hints = {0}, *res, *a;

    hints.ai_family = AF_INET;
    if (gethostname(host, sizeof host) || getaddrinfo(host, NULL, &hints, &res))
        return;
    for (a = res; a; a = a->ai_next) {
        char ip[INET_ADDRSTRLEN];
        struct sockaddr_in *sin = (struct sockaddr_in *)a->ai_addr;
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
        if (strncmp(ip, "127.", 4) == 0 || strncmp(ip, "169.254.", 8) == 0)
            continue;
        snprintf(g_web_url, sizeof g_web_url, "http://%s:%d", ip, WEB_PORT);
        /* Prefer the usual home-network ranges over VPN/virtual adapters. */
        if (strncmp(ip, "192.168.", 8) == 0 || strncmp(ip, "10.", 3) == 0)
            break;
    }
    freeaddrinfo(res);
}

/* Tiny read-only HTTP server: "/" is the page, "/data.json" the numbers. */
static DWORD WINAPI web_server(LPVOID unused)
{
    static Buf json;
    struct sockaddr_in addr = {0};
    SOCKET srv;
    WSADATA wsa;
    (void)unused;

    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return 0;
    find_lan_url();
    srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(WEB_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (srv == INVALID_SOCKET || bind(srv, (struct sockaddr *)&addr, sizeof addr) ||
        listen(srv, 8)) {
        g_web_url[0] = 0;
        WSACleanup();
        return 0;
    }

    while (WaitForSingleObject(g_stop, 0) == WAIT_TIMEOUT) {
        fd_set fds;
        struct timeval tv = {0, 500000};     /* wake up to check g_stop */
        SOCKET c;
        char req[1024];
        DWORD timeout = 2000;
        int n;

        FD_ZERO(&fds);
        FD_SET(srv, &fds);
        if (select(0, &fds, NULL, NULL, &tv) <= 0) continue;
        c = accept(srv, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof timeout);
        n = recv(c, req, sizeof req - 1, 0);
        if (n > 0) {
            /* Path without any "?query": "GET /data.json?x HTTP/1.1" -> "/data.json" */
            char *path = req + 4;
            req[n] = 0;
            if (strncmp(req, "GET ", 4) == 0)
                path[strcspn(path, " ?")] = 0;
            else
                path = NULL;
            if (!path) {
                web_send(c, "405 Method Not Allowed", "text/plain", "", 0);
            } else if (strcmp(path, "/data.json") == 0) {
                build_json(&json);
                web_send(c, "200 OK", "application/json", json.d, json.n);
            } else if (strcmp(path, "/") == 0) {
                web_send(c, "200 OK", "text/html; charset=utf-8", WEB_PAGE, sizeof WEB_PAGE - 1);
            } else {
                web_send(c, "404 Not Found", "text/plain", "", 0);
            }
        }
        closesocket(c);
    }
    closesocket(srv);
    WSACleanup();
    return 0;
}

/* ------------------------------------------------------------- drawing */

/* Green below 60%, yellow below 85%, red above. */
static COLORREF load_color(double pct)
{
    if (pct < 60.0) return RGB(80, 230, 120);
    if (pct < 85.0) return RGB(240, 210, 60);
    return RGB(255, 80, 80);
}

static COLORREF temp_color(int c)
{
    if (c < 60) return RGB(80, 230, 120);
    if (c < 80) return RGB(240, 210, 60);
    return RGB(255, 80, 80);
}

#define FAN_COLOR    RGB(120, 200, 255)
#define CLAUDE_COLOR RGB(230, 140, 90)
#define BG_COLOR     RGB(240, 210, 60)
#define IDLE_COLOR   RGB(150, 150, 150)

static const COLORREF CS_COLOR[] = { IDLE_COLOR, BG_COLOR, CLAUDE_COLOR };

#define MAX_SESSION_ROWS 8

static int session_rows(const Snapshot *s)
{
    return s->nclaude < MAX_SESSION_ROWS ? s->nclaude : MAX_SESSION_ROWS;
}

static int line_count(const Snapshot *s)
{
    return 2 + (s->gpu_ok ? 1 : 0) + s->nsens +
           (s->claude_ok ? 2 + session_rows(s) : 0);
}

static void draw_row(HDC hdc, int row, const wchar_t *label, const wchar_t *value, COLORREF col)
{
    int y = PAD + row * LINE_H;
    SetTextColor(hdc, RGB(200, 200, 200));
    SetTextAlign(hdc, TA_LEFT);
    TextOutW(hdc, 10, y, label, (int)wcslen(label));
    SetTextColor(hdc, col);
    SetTextAlign(hdc, TA_RIGHT);
    TextOutW(hdc, WIN_W - 10, y, value, (int)wcslen(value));
}

/* Like draw_row, but shortens the label with "…" so it never runs into the value. */
static void draw_row_fit(HDC hdc, int row, const wchar_t *label, const wchar_t *value, COLORREF col)
{
    SIZE ch, vs;
    wchar_t buf[160];
    int max;

    GetTextExtentPoint32W(hdc, L"M", 1, &ch);       /* monospace font */
    GetTextExtentPoint32W(hdc, value, (int)wcslen(value), &vs);
    max = (WIN_W - 20 - vs.cx - ch.cx) / ch.cx;
    if ((int)wcslen(label) > max && max > 1) {
        wcsncpy_s(buf, 160, label, max - 1);
        wcscat_s(buf, 160, L"…");
    } else {
        wcsncpy_s(buf, 160, label, _TRUNCATE);
    }
    draw_row(hdc, row, buf, value, col);
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    Snapshot s;
    wchar_t buf[64];
    int row = 0, i;

    EnterCriticalSection(&g_lock);
    s = g_snap;
    LeaveCriticalSection(&g_lock);

    GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SelectObject(hdc, g_font);
    SetBkMode(hdc, TRANSPARENT);

    swprintf(buf, 64, L"%.1f %%", s.cpu);
    draw_row(hdc, row++, L"CPU", buf, load_color(s.cpu));

    swprintf(buf, 64, L"%.1f / %.1f GB", s.mem_used_gb, s.mem_total_gb);
    draw_row(hdc, row++, L"RAM", buf, load_color((double)s.mem_load));

    if (s.gpu_ok) {
        swprintf(buf, 64, L"%u%%  %u°C  fan %u%%", s.gpu_load, s.gpu_temp, s.gpu_fan);
        draw_row(hdc, row++, L"GPU", buf, temp_color((int)s.gpu_temp));
    }

    for (i = 0; i < s.nsens; i++) {
        const Sensor *se = &s.sens[i];
        if (se->kind == KIND_TEMP) {
            swprintf(buf, 64, L"%d°C", se->value);
            draw_row(hdc, row++, se->label, buf, temp_color(se->value));
        } else {
            swprintf(buf, 64, L"%d rpm", se->value);
            draw_row(hdc, row++, se->label, buf, FAN_COLOR);
        }
    }

    if (s.claude_ok) {
        if (s.claude_bg)
            swprintf(buf, 64, L"%d busy / %d bg / %d idle", s.claude_busy, s.claude_bg, s.claude_idle);
        else
            swprintf(buf, 64, L"%d busy / %d idle", s.claude_busy, s.claude_idle);
        draw_row(hdc, row++, L"Claude", buf, s.claude_busy ? CLAUDE_COLOR : IDLE_COLOR);

        /* One row per session: busy, then bg, then idle. */
        {
            int st, shown = 0;
            wchar_t label[140];
            for (st = CS_BUSY; st >= CS_IDLE; st--) {
                for (i = 0; i < s.nclaude && shown < MAX_SESSION_ROWS; i++) {
                    const ClaudeSession *cs = &s.claude[i];
                    if (cs->state != st) continue;
                    swprintf(label, 140, L"  %s", cs->label);
                    if (cs->agents)
                        swprintf(buf, 64, L"%s +%d", CS_NAME[st], cs->agents);
                    else
                        wcscpy_s(buf, 64, CS_NAME[st]);
                    draw_row_fit(hdc, row++, label, buf, CS_COLOR[st]);
                    shown++;
                }
            }
        }

        swprintf(buf, 64, L"%d", s.claude_agents);
        draw_row(hdc, row++, L"Background", buf, s.claude_agents ? CLAUDE_COLOR : IDLE_COLOR);
    }
    EndPaint(hwnd, &ps);
}

/* ------------------------------------------------------------ tray icon */

/* Render a number into a small-icon-sized ARGB icon. */
static HICON make_number_icon(int n, COLORREF col)
{
    int sz = GetSystemMetrics(SM_CXSMICON), i;
    BITMAPINFO bi = {0};
    DWORD *px;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP color, mask;
    HFONT font, old_font;
    RECT rc = {0, 0, sz, sz};
    wchar_t txt[8];
    ICONINFO ii = {0};
    HICON icon;

    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = sz;
    bi.bmiHeader.biHeight = -sz;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    mask = CreateBitmap(sz, sz, 1, 1, NULL);
    SelectObject(dc, color);
    ZeroMemory(px, sz * sz * 4);

    /* Grayscale antialiasing, so the glyph coverage can become alpha. */
    font = CreateFontW(n >= 100 ? sz * 3 / 4 : sz, 0, 0, 0, FW_BOLD, 0, 0, 0,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    swprintf(txt, 8, L"%d", n);
    DrawTextW(dc, txt, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    GdiFlush();

    for (i = 0; i < sz * sz; i++) {
        DWORD a = px[i] & 0xFF;
        px[i] = (a << 24) | (GetRValue(col) << 16) | (GetGValue(col) << 8) | GetBValue(col);
        if (!a) px[i] = 0;
    }

    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    icon = CreateIconIndirect(&ii);

    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteObject(color);
    DeleteObject(mask);
    DeleteDC(dc);
    return icon;
}

static void tray_update(int add)
{
    NOTIFYICONDATAW nid = {0};
    HICON old = g_tray_icon;
    Snapshot s;
    wchar_t line[64];
    int i;

    EnterCriticalSection(&g_lock);
    s = g_snap;
    LeaveCriticalSection(&g_lock);

    g_tray_icon = make_number_icon((int)(s.cpu + 0.5), load_color(s.cpu));

    nid.cbSize = sizeof nid;
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_tray_icon;

    swprintf(nid.szTip, 128, L"CPU %.0f%%", s.cpu);
    if (s.cpu_temp >= 0) {
        swprintf(line, 64, L"  %d°C", s.cpu_temp);
        wcscat_s(nid.szTip, 128, line);
    }
    swprintf(line, 64, L"\nRAM %.1f/%.0f GB", s.mem_used_gb, s.mem_total_gb);
    wcscat_s(nid.szTip, 128, line);
    if (s.gpu_ok) {
        swprintf(line, 64, L"\nGPU %u%%  %u°C", s.gpu_load, s.gpu_temp);
        wcscat_s(nid.szTip, 128, line);
    }
    if (s.claude_ok) {
        swprintf(line, 64, L"\nClaude %d busy, %d agents", s.claude_busy, s.claude_agents);
        wcscat_s(nid.szTip, 128, line);
    }
    for (i = 0; i < s.nsens; i++) {
        if (s.sens[i].kind != KIND_FAN) continue;
        swprintf(line, 64, L"\n%s %d", s.sens[i].label, s.sens[i].value);
        if (wcslen(nid.szTip) + wcslen(line) >= 128) break;
        wcscat_s(nid.szTip, 128, line);
    }

    if (!Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &nid) && !add)
        Shell_NotifyIconW(NIM_ADD, &nid);       /* e.g. taskbar restarted */
    if (old) DestroyIcon(old);
}

static void tray_remove(void)
{
    NOTIFYICONDATAW nid = {0};
    nid.cbSize = sizeof nid;
    nid.hWnd = g_hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (g_tray_icon) DestroyIcon(g_tray_icon);
    g_tray_icon = NULL;
}

static void toggle_overlay(HWND hwnd)
{
    ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOWNOACTIVATE);
}

/* Copy src to dst, doubling '&' so menus don't treat it as a mnemonic. */
static void menu_escape(wchar_t *dst, size_t cap, const wchar_t *src)
{
    size_t n = 0;
    for (; *src && n + 2 < cap; src++) {
        if (*src == L'&') dst[n++] = L'&';
        dst[n++] = *src;
    }
    dst[n] = 0;
}

/* One line per Claude session, busy ones first. */
static void add_claude_items(HMENU m)
{
    Snapshot s;
    wchar_t esc[256], item[320];
    int st, i, any = 0;

    EnterCriticalSection(&g_lock);
    s = g_snap;
    LeaveCriticalSection(&g_lock);
    if (!s.claude_ok) return;

    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, L"Claude sessions");
    for (st = CS_BUSY; st >= CS_IDLE; st--) {
        for (i = 0; i < s.nclaude; i++) {
            const ClaudeSession *cs = &s.claude[i];
            if (cs->state != st) continue;
            menu_escape(esc, 256, cs->label);
            if (cs->agents)
                swprintf(item, 320, L"%s\t%s  +%d in background", esc, CS_NAME[st], cs->agents);
            else
                swprintf(item, 320, L"%s\t%s", esc, CS_NAME[st]);
            AppendMenuW(m, MF_STRING | (st == CS_BUSY ? MF_CHECKED : 0), IDM_SESSION, item);
            any = 1;
        }
    }
    if (!any)
        AppendMenuW(m, MF_STRING | MF_DISABLED, 0, L"(none)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
}

static void tray_menu(HWND hwnd)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    add_claude_items(m);
    if (g_web_url[0]) {
        wchar_t item[96];
        swprintf(item, 96, L"Phone: %hs", g_web_url);
        AppendMenuW(m, MF_STRING | MF_DISABLED, 0, item);
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(m, MF_STRING, IDM_TOGGLE,
                IsWindowVisible(hwnd) ? L"Hide overlay\tCtrl+Alt+O" : L"Show overlay\tCtrl+Alt+O");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_QUIT, L"Quit\tCtrl+Alt+Q");
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);          /* so the menu closes on click-away */
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

/* ---------------------------------------------------------------- window */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_APP_DATA: {
        int h;
        EnterCriticalSection(&g_lock);
        h = PAD * 2 + line_count(&g_snap) * LINE_H;
        LeaveCriticalSection(&g_lock);
        if (h != g_win_h) {
            g_win_h = h;
            SetWindowPos(hwnd, NULL, 0, 0, WIN_W, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        /* Re-assert topmost in case another topmost window covered us. */
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        tray_update(0);
        return 0;
    }
    case WM_APP_TRAY:
        if (lp == WM_LBUTTONUP)
            toggle_overlay(hwnd);
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
            tray_menu(hwnd);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDM_TOGGLE) toggle_overlay(hwnd);
        else if (LOWORD(wp) == IDM_QUIT) DestroyWindow(hwnd);
        return 0;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_HOTKEY:
        if (wp == HK_TOGGLE) toggle_overlay(hwnd);
        else if (wp == HK_QUIT) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        UnregisterHotKey(hwnd, HK_TOGGLE);
        UnregisterHotKey(hwnd, HK_QUIT);
        tray_remove();
        PostQuitMessage(0);
        return 0;
    }
    if (msg == g_taskbar_created && msg) {   /* Explorer restarted */
        tray_update(1);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
    WNDCLASSW wc = {0};
    HANDLE thread, web;
    MSG msg;
    (void)prev; (void)cmd; (void)show;

    SetProcessDPIAware();
    InitializeCriticalSection(&g_lock);
    GetEnvironmentVariableW(L"USERPROFILE", g_claude_dir, MAX_PATH);
    wcscat_s(g_claude_dir, MAX_PATH, L"\\.claude\\hw-overlay");
    g_stop = CreateEventW(NULL, TRUE, FALSE, NULL);

    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"HwOverlay";
    RegisterClassW(&wc);

    g_font = CreateFontW(20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");

    /* LAYERED+TRANSPARENT = see-through and click-through,
       TOOLWINDOW = no taskbar button, NOACTIVATE = never takes focus. */
    g_win_h = PAD * 2 + 2 * LINE_H;
    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"HW Overlay", WS_POPUP,
        MARGIN, MARGIN, WIN_W, g_win_h, NULL, NULL, inst, NULL);
    if (!g_hwnd) {
        MessageBoxW(NULL, L"Could not create window.", L"Overlay", MB_ICONERROR);
        return 1;
    }
    SetLayeredWindowAttributes(g_hwnd, 0, 190, LWA_ALPHA);   /* ~75% opaque */

    /* Running elevated: let Explorer (lower integrity) reach us. */
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(g_hwnd, g_taskbar_created, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(g_hwnd, WM_APP_TRAY, MSGFLT_ALLOW, NULL);

    if (!RegisterHotKey(g_hwnd, HK_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'O') ||
        !RegisterHotKey(g_hwnd, HK_QUIT,   MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q'))
        MessageBoxW(NULL, L"Hotkey Ctrl+Alt+O / Ctrl+Alt+Q is taken by another app.\n"
                    L"Quit from the tray icon instead.", L"Overlay", MB_ICONWARNING);

    tray_update(1);
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    thread = CreateThread(NULL, 0, sampler, NULL, 0, NULL);
    web = CreateThread(NULL, 0, web_server, NULL, 0, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SetEvent(g_stop);
    WaitForSingleObject(thread, 5000);
    WaitForSingleObject(web, 2000);
    DeleteObject(g_font);
    return 0;
}
