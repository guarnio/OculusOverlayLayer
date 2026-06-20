#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <vector>

#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

// ============================ logging ============================
static void Log(const char* fmt, ...) {
    char path[MAX_PATH]; const char* tmp = getenv("TEMP"); if (!tmp) tmp = "C:\\";
    snprintf(path, sizeof(path), "%s\\MyOverlayLayer.log", tmp);
    FILE* f = fopen(path, "a"); if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fclose(f);
}
static bool XR_OK(XrResult r, const char* w) { if (XR_FAILED(r)) { Log("%s FAILED %d\n", w, (int)r); return false; } return true; }
static inline float fmax3(float a, float b, float c) { float m = a > b ? a : b; return m > c ? m : c; }

// ============================ chain pointers ============================
static PFN_xrGetInstanceProcAddr g_gipa = nullptr;
static XrInstance g_instance = XR_NULL_HANDLE;
static PFN_xrCreateSession             real_xrCreateSession = nullptr;
static PFN_xrEndFrame                  real_xrEndFrame = nullptr;
static PFN_xrEnumerateSwapchainFormats pEnumFormats = nullptr;
static PFN_xrCreateSwapchain           pCreateSwapchain = nullptr;
static PFN_xrEnumerateSwapchainImages  pEnumImages = nullptr;
static PFN_xrAcquireSwapchainImage     pAcquire = nullptr;
static PFN_xrWaitSwapchainImage        pWait = nullptr;
static PFN_xrReleaseSwapchainImage     pRelease = nullptr;
static PFN_xrCreateReferenceSpace      pCreateRefSpace = nullptr;

// ============================ config (overlay.ini) ============================
static float cfgPosX = 0.35f, cfgPosY = 0.25f, cfgPosZ = -1.0f;
static float cfgSizeX = 0.26f, cfgSizeY = 0.145f;
static int   cfgTexW = 384, cfgTexH = 210;
static int   cfgBgR = 0, cfgBgG = 0, cfgBgB = 0, cfgBgA = 60;
static int   cfgTR = 120, cfgTG = 230, cfgTB = 255;
static int   cfgFontSize = 40;
static char  cfgFontName[64] = "Consolas";
static char  cfgLock[16] = "world";
static int   cfgRefreshMs = 250;
static char  cfgHotkey[64] = "Ctrl+Alt+F2";

// ============================ runtime state ============================
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static XrSession   g_session = XR_NULL_HANDLE;
static XrSpace     g_space = XR_NULL_HANDLE;
static XrSwapchain g_swapchain = XR_NULL_HANDLE;
static std::vector<ID3D11Texture2D*> g_textures;
static bool g_isBGRA = true, g_ready = false, g_initFailed = false, g_loggedOnce = false;

static HDC g_memDC = nullptr; static HBITMAP g_dib = nullptr; static void* g_dibBits = nullptr; static HFONT g_font = nullptr;
static std::vector<uint8_t> g_upload;

static ULONGLONG g_lastRender = 0;
static bool      g_haveImage = false;

// FPS
static ULONGLONG g_fpsLast = 0; static int g_fpsCount = 0; static float g_fps = 0.0f;

// hotkey toggle
static int  g_hotMods = 0;       // 1=ctrl 2=alt 4=shift 8=win
static int  g_hotVk = 0;
static bool g_hotPrev = false;
static bool g_visible = true;

// ============================ NVIDIA NVML ============================
typedef int   nvmlReturn_t;
typedef void* nvmlDevice_t;
struct nvmlUtilization_t { unsigned int gpu; unsigned int memory; };
typedef nvmlReturn_t(*PFN_nvmlInit)();
typedef nvmlReturn_t(*PFN_nvmlGetHandle)(unsigned int, nvmlDevice_t*);
typedef nvmlReturn_t(*PFN_nvmlTemp)(nvmlDevice_t, int, unsigned int*);
typedef nvmlReturn_t(*PFN_nvmlUtil)(nvmlDevice_t, nvmlUtilization_t*);
static HMODULE      g_nvml = nullptr;
static nvmlDevice_t g_gpu = nullptr;
static PFN_nvmlTemp p_nvmlTemp = nullptr;
static PFN_nvmlUtil p_nvmlUtil = nullptr;
static bool         g_nvmlOk = false;
static DWORD        g_lastPoll = 0;
static unsigned int g_cachedUtil = 0, g_cachedTemp = 0;

// FPS graph
static int   cfgGraphOn = 1;
static int   cfgGraphSeconds = 30;
static int   cfgGraphH = 110;          // graph height in texture pixels
static float cfgGraphMax = 100.0f;     // FPS at top of graph (0 = auto-scale)
static std::vector<float> g_hist;
static int   g_histN = 0, g_histPos = 0, g_histCount = 0;
static HPEN  g_penGraph = nullptr, g_penAxis = nullptr;

static void InitNvml() {
    g_nvml = LoadLibraryA("nvml.dll");
    if (!g_nvml) g_nvml = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if (!g_nvml) { Log("nvml.dll not found\n"); return; }
    auto init = (PFN_nvmlInit)GetProcAddress(g_nvml, "nvmlInit_v2");
    if (!init)   init = (PFN_nvmlInit)GetProcAddress(g_nvml, "nvmlInit");
    auto handle = (PFN_nvmlGetHandle)GetProcAddress(g_nvml, "nvmlDeviceGetHandleByIndex_v2");
    if (!handle) handle = (PFN_nvmlGetHandle)GetProcAddress(g_nvml, "nvmlDeviceGetHandleByIndex");
    p_nvmlTemp = (PFN_nvmlTemp)GetProcAddress(g_nvml, "nvmlDeviceGetTemperature");
    p_nvmlUtil = (PFN_nvmlUtil)GetProcAddress(g_nvml, "nvmlDeviceGetUtilizationRates");
    if (!init || !handle || !p_nvmlTemp || !p_nvmlUtil) { Log("NVML symbols missing\n"); return; }
    if (init() != 0) { Log("nvmlInit failed\n"); return; }
    if (handle(0, &g_gpu) != 0) { Log("nvml handle failed\n"); return; }
    g_nvmlOk = true; Log("NVML ready\n");
}

// ============================ config helpers ============================
static void GetIniPath(char* out, size_t n) {
    HMODULE h = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&GetIniPath, &h);
    char p[MAX_PATH]; GetModuleFileNameA(h, p, MAX_PATH);
    char* s = strrchr(p, '\\'); if (s) *(s + 1) = 0;
    snprintf(out, n, "%soverlay.ini", p);
}
static float IniF(const char* ini, const char* key, float def) {
    char buf[64], d[64]; snprintf(d, sizeof d, "%f", def);
    GetPrivateProfileStringA("Overlay", key, d, buf, sizeof buf, ini); return (float)atof(buf);
}
static int IniI(const char* ini, const char* key, int def) { return (int)GetPrivateProfileIntA("Overlay", key, def, ini); }

static int TokenToVk(const char* tok) {
    if (tok[0] == 'F' && tok[1] >= '1' && tok[1] <= '9') { int n = atoi(tok + 1); if (n >= 1 && n <= 24) return VK_F1 + (n - 1); }
    if (strlen(tok) == 1) {
        char c = tok[0];
        if (c >= 'A' && c <= 'Z') return c;      // VK letter codes == ASCII uppercase
        if (c >= '0' && c <= '9') return c;      // VK digit codes == ASCII
    }
    if (!strcmp(tok, "SPACE"))  return VK_SPACE;
    if (!strcmp(tok, "TAB"))    return VK_TAB;
    if (!strcmp(tok, "HOME"))   return VK_HOME;
    if (!strcmp(tok, "END"))    return VK_END;
    if (!strcmp(tok, "INSERT")) return VK_INSERT;
    if (!strcmp(tok, "DELETE") || !strcmp(tok, "DEL")) return VK_DELETE;
    if (!strcmp(tok, "PGUP") || !strcmp(tok, "PAGEUP"))   return VK_PRIOR;
    if (!strcmp(tok, "PGDN") || !strcmp(tok, "PAGEDOWN")) return VK_NEXT;
    if (!strcmp(tok, "UP"))     return VK_UP;
    if (!strcmp(tok, "DOWN"))   return VK_DOWN;
    if (!strcmp(tok, "LEFT"))   return VK_LEFT;
    if (!strcmp(tok, "RIGHT"))  return VK_RIGHT;
    return 0;
}
static void ParseHotkey(const char* s) {
    g_hotMods = 0; g_hotVk = 0;
    char buf[64]; strncpy(buf, s, sizeof buf); buf[sizeof buf - 1] = 0;
    for (char* p = buf; *p; ++p) *p = (char)toupper((unsigned char)*p);
    char* ctx = nullptr;
    for (char* tok = strtok_s(buf, "+ ", &ctx); tok; tok = strtok_s(nullptr, "+ ", &ctx)) {
        if (!strcmp(tok, "CTRL") || !strcmp(tok, "CONTROL")) g_hotMods |= 1;
        else if (!strcmp(tok, "ALT"))   g_hotMods |= 2;
        else if (!strcmp(tok, "SHIFT")) g_hotMods |= 4;
        else if (!strcmp(tok, "WIN"))   g_hotMods |= 8;
        else { int vk = TokenToVk(tok); if (vk) g_hotVk = vk; }
    }
    Log("Hotkey: '%s' -> mods=%d vk=0x%02X\n", s, g_hotMods, g_hotVk);
}
static void LoadConfig() {
    char ini[MAX_PATH]; GetIniPath(ini, sizeof ini);
    Log("Config: %s\n", ini);
    cfgPosX = IniF(ini, "PosX", cfgPosX);   cfgPosY = IniF(ini, "PosY", cfgPosY);   cfgPosZ = IniF(ini, "PosZ", cfgPosZ);
    cfgSizeX = IniF(ini, "SizeX", cfgSizeX); cfgSizeY = IniF(ini, "SizeY", cfgSizeY);
    cfgTexW = IniI(ini, "TexW", cfgTexW);    cfgTexH = IniI(ini, "TexH", cfgTexH);
    cfgBgR = IniI(ini, "BgR", cfgBgR); cfgBgG = IniI(ini, "BgG", cfgBgG); cfgBgB = IniI(ini, "BgB", cfgBgB); cfgBgA = IniI(ini, "BgAlpha", cfgBgA);
    cfgTR = IniI(ini, "TextR", cfgTR); cfgTG = IniI(ini, "TextG", cfgTG); cfgTB = IniI(ini, "TextB", cfgTB);
    cfgFontSize = IniI(ini, "FontSize", cfgFontSize);
    cfgGraphOn = IniI(ini, "Graph", cfgGraphOn);
    cfgGraphSeconds = IniI(ini, "GraphSeconds", cfgGraphSeconds);
    cfgGraphH = IniI(ini, "GraphHeight", cfgGraphH);
    cfgGraphMax = IniF(ini, "GraphMaxFps", cfgGraphMax);
    GetPrivateProfileStringA("Overlay", "FontName", cfgFontName, cfgFontName, sizeof cfgFontName, ini);
    GetPrivateProfileStringA("Overlay", "Lock", cfgLock, cfgLock, sizeof cfgLock, ini);
    cfgRefreshMs = IniI(ini, "RefreshMs", cfgRefreshMs);
    GetPrivateProfileStringA("Overlay", "Hotkey", cfgHotkey, cfgHotkey, sizeof cfgHotkey, ini);
    ParseHotkey(cfgHotkey);
}

// ============================ hotkey ============================
static void PollHotkey() {
    if (g_hotVk == 0) return;
    bool down = true;
    if (g_hotMods & 1) down &= (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    if (g_hotMods & 2) down &= (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    if (g_hotMods & 4) down &= (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (g_hotMods & 8) down &= ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
    down &= (GetAsyncKeyState(g_hotVk) & 0x8000) != 0;
    if (down && !g_hotPrev) {
        g_visible = !g_visible;
        if (g_visible) g_lastRender = 0;           // force immediate redraw when shown
        Log("Panel toggled: %s\n", g_visible ? "ON" : "OFF");
    }
    g_hotPrev = down;
}

// ============================ overlay setup ============================
static void InitOverlay() {
    if (g_ready || g_initFailed) return;
    if (g_session == XR_NULL_HANDLE || g_device == nullptr) return;
    LoadConfig();

    bool headLocked = (_stricmp(cfgLock, "head") == 0);
    XrReferenceSpaceCreateInfo si{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    si.referenceSpaceType = headLocked ? XR_REFERENCE_SPACE_TYPE_VIEW : XR_REFERENCE_SPACE_TYPE_LOCAL;
    si.poseInReferenceSpace.orientation.w = 1.0f;
    Log("Lock mode: %s\n", headLocked ? "head" : "world");
    if (!XR_OK(pCreateRefSpace(g_session, &si, &g_space), "CreateRefSpace")) { g_initFailed = true; return; }

    uint32_t fc = 0; pEnumFormats(g_session, 0, &fc, nullptr);
    std::vector<int64_t> fmts(fc); pEnumFormats(g_session, fc, &fc, fmts.data());
    int64_t pref[] = { DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                       DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB };
    int64_t chosen = fmts.empty() ? DXGI_FORMAT_B8G8R8A8_UNORM : fmts[0]; bool got = false;
    for (int64_t pf : pref) { for (int64_t f : fmts) if (f == pf) { chosen = pf; got = true; break; } if (got) break; }
    g_isBGRA = (chosen == DXGI_FORMAT_B8G8R8A8_UNORM || chosen == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
    Log("Format %lld (BGRA=%d)\n", (long long)chosen, g_isBGRA);

    XrSwapchainCreateInfo sc{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sc.format = chosen; sc.sampleCount = 1; sc.width = cfgTexW; sc.height = cfgTexH;
    sc.faceCount = 1; sc.arraySize = 1; sc.mipCount = 1;
    if (!XR_OK(pCreateSwapchain(g_session, &sc, &g_swapchain), "CreateSwapchain")) { g_initFailed = true; return; }

    uint32_t ic = 0; pEnumImages(g_swapchain, 0, &ic, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(ic, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
    pEnumImages(g_swapchain, ic, &ic, (XrSwapchainImageBaseHeader*)imgs.data());
    g_textures.resize(ic);
    for (uint32_t i = 0; i < ic; i++) g_textures[i] = imgs[i].texture;

    BITMAPINFO bmi{}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = cfgTexW; bmi.bmiHeader.biHeight = -cfgTexH;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    g_memDC = CreateCompatibleDC(NULL);
    g_dib = CreateDIBSection(g_memDC, &bmi, DIB_RGB_COLORS, &g_dibBits, NULL, 0);
    SelectObject(g_memDC, g_dib);
    g_font = CreateFontA(cfgFontSize, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
        OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, cfgFontName);
    SelectObject(g_memDC, g_font);
    SetBkMode(g_memDC, TRANSPARENT);
    SetTextColor(g_memDC, RGB(255, 255, 255));
    g_upload.resize((size_t)cfgTexW * cfgTexH * 4);

    g_penGraph = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    g_penAxis = CreatePen(PS_SOLID, 1, RGB(90, 90, 90));
    g_histN = cfgGraphSeconds * 1000 / (cfgRefreshMs > 0 ? cfgRefreshMs : 250);
    if (g_histN < 2) g_histN = 2;
    g_hist.assign(g_histN, 0.0f);
    g_histPos = 0; g_histCount = 0;

    InitNvml();
    Log("Overlay ready: %u images, %dx%d.\n", ic, cfgTexW, cfgTexH);
    g_ready = true;
}

// ============================ draw panel contents ============================
static void DrawContent() {
    memset(g_dibBits, 0, (size_t)cfgTexW * cfgTexH * 4);

    int graphArea = (cfgGraphOn ? cfgGraphH : 0);
    int textBottom = cfgTexH - graphArea;     // text lives above the graph

    // ---- text ----
    SYSTEMTIME t; GetLocalTime(&t);
    char text[128];
    if (g_nvmlOk) {
        DWORD now = GetTickCount();
        if (now - g_lastPoll > 500) {
            unsigned int temp = 0; nvmlUtilization_t u{};
            if (p_nvmlTemp(g_gpu, 0, &temp) == 0) g_cachedTemp = temp;
            if (p_nvmlUtil(g_gpu, &u) == 0)       g_cachedUtil = u.gpu;
            g_lastPoll = now;
        }
        snprintf(text, sizeof text, "%02d:%02d:%02d\r\nGPU %u%%  %u\xB0" "C\r\nFPS %.0f",
            t.wHour, t.wMinute, t.wSecond, g_cachedUtil, g_cachedTemp, g_fps);
    }
    else {
        snprintf(text, sizeof text, "%02d:%02d:%02d\r\nGPU n/a\r\nFPS %.0f",
            t.wHour, t.wMinute, t.wSecond, g_fps);
    }
    RECT calc{ 0,0,cfgTexW,textBottom };
    DrawTextA(g_memDC, text, -1, &calc, DT_CENTER | DT_CALCRECT);
    int th = calc.bottom - calc.top;
    RECT draw{ 0, (textBottom - th) / 2, cfgTexW, textBottom };
    DrawTextA(g_memDC, text, -1, &draw, DT_CENTER | DT_NOCLIP);

    // ---- FPS graph ----
    if (cfgGraphOn) {
        g_hist[g_histPos] = g_fps;                       // push newest sample
        g_histPos = (g_histPos + 1) % g_histN;
        if (g_histCount < g_histN) g_histCount++;

        int m = 6;
        int gx0 = m, gx1 = cfgTexW - m, gy0 = textBottom + 2, gy1 = cfgTexH - m;

        HGDIOBJ ob = SelectObject(g_memDC, GetStockObject(NULL_BRUSH));
        HGDIOBJ op = SelectObject(g_memDC, g_penAxis);
        Rectangle(g_memDC, gx0, gy0, gx1, gy1);          // faint frame

        float mx = cfgGraphMax;
        if (mx <= 0) { mx = 1.0f; for (int k = 0; k < g_histCount; k++) if (g_hist[k] > mx) mx = g_hist[k]; }

        if (g_histCount >= 2) {
            SelectObject(g_memDC, g_penGraph);
            int gw = gx1 - gx0, gh = gy1 - gy0;
            for (int k = 0; k < g_histCount; k++) {
                int idx = (g_histPos - g_histCount + k + g_histN) % g_histN;   // oldest -> newest
                float r = g_hist[idx] / mx; if (r < 0) r = 0; if (r > 1) r = 1;
                int x = gx0 + k * gw / (g_histCount - 1);
                int y = gy1 - (int)(r * gh);
                if (k == 0) MoveToEx(g_memDC, x, y, NULL); else LineTo(g_memDC, x, y);
            }
        }
        SelectObject(g_memDC, op);
        SelectObject(g_memDC, ob);
    }

    GdiFlush();

    // ---- composite to premultiplied-alpha upload buffer ----
    const uint8_t* src = (const uint8_t*)g_dibBits;
    uint8_t* dst = g_upload.data();
    float bgA = cfgBgA / 255.0f;
    float pbR = (cfgBgR / 255.0f) * bgA, pbG = (cfgBgG / 255.0f) * bgA, pbB = (cfgBgB / 255.0f) * bgA;
    float tR = cfgTR / 255.0f, tG = cfgTG / 255.0f, tB = cfgTB / 255.0f;
    int n = cfgTexW * cfgTexH;
    for (int i = 0; i < n; i++) {
        float b = src[i * 4 + 0], g = src[i * 4 + 1], r = src[i * 4 + 2];
        float cov = fmax3(r, g, b) / 255.0f;
        float oA = cov + bgA * (1 - cov);
        float oR = tR * cov + pbR * (1 - cov);
        float oG = tG * cov + pbG * (1 - cov);
        float oB = tB * cov + pbB * (1 - cov);
        uint8_t R = (uint8_t)(oR * 255), G = (uint8_t)(oG * 255), B = (uint8_t)(oB * 255), A = (uint8_t)(oA * 255);
        if (g_isBGRA) { dst[i * 4 + 0] = B; dst[i * 4 + 1] = G; dst[i * 4 + 2] = R; dst[i * 4 + 3] = A; }
        else { dst[i * 4 + 0] = R; dst[i * 4 + 1] = G; dst[i * 4 + 2] = B; dst[i * 4 + 3] = A; }
    }
}

// ============================ hooks ============================
static XrResult XRAPI_CALL MyCreateSession(XrInstance inst, const XrSessionCreateInfo* ci, XrSession* sess) {
    const XrBaseInStructure* p = (const XrBaseInStructure*)ci->next;
    while (p) {
        if (p->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            g_device = ((const XrGraphicsBindingD3D11KHR*)p)->device;
            if (g_device) g_device->GetImmediateContext(&g_context);
            Log("Captured D3D11 device %p\n", (void*)g_device);
        }
        p = p->next;
    }
    XrResult r = real_xrCreateSession(inst, ci, sess);
    if (XR_SUCCEEDED(r)) { g_session = *sess; Log("Session %p\n", (void*)g_session); }
    return r;
}

static XrResult XRAPI_CALL MyEndFrame(XrSession session, const XrFrameEndInfo* fi) {
    InitOverlay();

    // FPS: this hook fires once per rendered frame
    g_fpsCount++;
    ULONGLONG now = GetTickCount64();
    if (g_fpsLast == 0) g_fpsLast = now;
    if (now - g_fpsLast >= 500) { g_fps = g_fpsCount * 1000.0f / (now - g_fpsLast); g_fpsCount = 0; g_fpsLast = now; }

    PollHotkey();
    if (!g_ready || !g_visible) return real_xrEndFrame(session, fi);

    // Re-render only a few times per second
    if (!g_haveImage || now - g_lastRender >= (ULONGLONG)cfgRefreshMs) {
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (XR_SUCCEEDED(pAcquire(g_swapchain, &ai, &idx))) {
            XrSwapchainImageWaitInfo wi{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = XR_INFINITE_DURATION;
            pWait(g_swapchain, &wi);
            DrawContent();
            g_context->UpdateSubresource(g_textures[idx], 0, nullptr, g_upload.data(), cfgTexW * 4, 0);
            XrSwapchainImageReleaseInfo ri{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            pRelease(g_swapchain, &ri);
            g_lastRender = now; g_haveImage = true;
        }
    }
    if (!g_haveImage) return real_xrEndFrame(session, fi);

    XrCompositionLayerQuad q{ XR_TYPE_COMPOSITION_LAYER_QUAD };
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = g_space; q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = g_swapchain;
    q.subImage.imageRect.offset = { 0, 0 };
    q.subImage.imageRect.extent = { cfgTexW, cfgTexH };
    q.subImage.imageArrayIndex = 0;
    q.pose.orientation = { 0, 0, 0, 1 };
    q.pose.position = { cfgPosX, cfgPosY, cfgPosZ };
    q.size = { cfgSizeX, cfgSizeY };

    const XrCompositionLayerBaseHeader* layers[16];
    uint32_t lc = 0;
    for (uint32_t i = 0; i < fi->layerCount && lc < 15; i++) layers[lc++] = fi->layers[i];
    layers[lc++] = (const XrCompositionLayerBaseHeader*)&q;

    XrFrameEndInfo ni = *fi; ni.layerCount = lc; ni.layers = layers;
    if (!g_loggedOnce) { Log("First panel submitted!\n"); g_loggedOnce = true; }
    return real_xrEndFrame(session, &ni);
}

static XrResult XRAPI_CALL MyGetInstanceProcAddr(XrInstance inst, const char* name, PFN_xrVoidFunction* fn) {
    if (!strcmp(name, "xrEndFrame")) { *fn = (PFN_xrVoidFunction)MyEndFrame;      return XR_SUCCESS; }
    if (!strcmp(name, "xrCreateSession")) { *fn = (PFN_xrVoidFunction)MyCreateSession; return XR_SUCCESS; }
    return g_gipa(inst, name, fn);
}

static XrResult XRAPI_CALL MyCreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* li, XrInstance* inst) {
    Log("=== layer loading ===\n");
    XrApiLayerCreateInfo nx = *li; nx.nextInfo = li->nextInfo->next;
    g_gipa = li->nextInfo->nextGetInstanceProcAddr;
    PFN_xrCreateApiLayerInstance nc = li->nextInfo->nextCreateApiLayerInstance;
    XrResult r = nc(info, &nx, inst);
    if (XR_FAILED(r)) { Log("downstream create failed %d\n", r); return r; }
    g_instance = *inst;
    auto R = [&](const char* n, void* f) { g_gipa(g_instance, n, (PFN_xrVoidFunction*)f); };
    R("xrCreateSession", &real_xrCreateSession); R("xrEndFrame", &real_xrEndFrame);
    R("xrEnumerateSwapchainFormats", &pEnumFormats); R("xrCreateSwapchain", &pCreateSwapchain);
    R("xrEnumerateSwapchainImages", &pEnumImages);  R("xrAcquireSwapchainImage", &pAcquire);
    R("xrWaitSwapchainImage", &pWait); R("xrReleaseSwapchainImage", &pRelease);
    R("xrCreateReferenceSpace", &pCreateRefSpace);
    Log("Functions resolved.\n");
    return r;
}

extern "C" __declspec(dllexport) XrResult XRAPI_CALL
xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo*, const char* layerName, XrNegotiateApiLayerRequest* req) {
    Log("negotiate: %s\n", layerName ? layerName : "(null)");
    req->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    req->layerApiVersion = XR_CURRENT_API_VERSION;
    req->getInstanceProcAddr = MyGetInstanceProcAddr;
    req->createApiLayerInstance = MyCreateApiLayerInstance;
    return XR_SUCCESS;
}