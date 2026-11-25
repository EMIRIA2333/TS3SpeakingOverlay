// printwin.cpp - capture a window via PrintWindow (works for layered windows
// whose surface is kept by DWM) and save it as PNG.
// usage: printwin.exe <out.png> <className|hwnd> [pad]
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <propidl.h>
#include <gdiplus.h>
#include <string>
#include <cstdio>
#include <cwctype>

using namespace Gdiplus;

static bool GetEncoderClsid(const wchar_t* mime, CLSID* out)
{
    UINT num = 0, size = 0;
    if (GetImageEncodersSize(&num, &size) != Ok || size == 0) return false;
    ImageCodecInfo* info = (ImageCodecInfo*)malloc(size);
    if (!info) return false;
    bool ok = false;
    if (GetImageEncoders(num, size, info) == Ok) {
        for (UINT i = 0; i < num; ++i) {
            if (wcscmp(info[i].MimeType, mime) == 0) { *out = info[i].Clsid; ok = true; break; }
        }
    }
    free(info);
    return ok;
}

static HWND g_target = NULL;
static std::wstring g_want;

static BOOL CALLBACK EnumProc(HWND h, LPARAM)
{
    wchar_t cls[256] = {0};
    GetClassNameW(h, cls, 256);
    if (g_want == cls && IsWindowVisible(h)) { g_target = h; return FALSE; }
    return TRUE;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3) { wprintf(L"usage: printwin.exe <out.png> <className|hwnd> [pad]\n"); return 2; }
    const wchar_t* outPath = argv[1];
    g_want = argv[2];
    int pad = (argc > 3) ? _wtoi(argv[3]) : 0;

    HWND h = NULL;
    if (!g_want.empty() && iswdigit(g_want[0])) {
        wchar_t* end = NULL;
        unsigned long long v = _wcstoui64(g_want.c_str(), &end, 0);
        h = (HWND)(ULONG_PTR)v;
    }
    if (!h) h = FindWindowW(g_want.c_str(), NULL);
    if (!h) { EnumWindows(EnumProc, 0); h = g_target; }
    if (!h || !IsWindow(h)) { wprintf(L"window not found\n"); return 1; }

    RECT r; GetWindowRect(h, &r);
    int w = r.right - r.left, ht = r.bottom - r.top;
    int cw = w + pad * 2, ch = ht + pad * 2;
    wprintf(L"hwnd=%p rect=%ld,%ld,%ld,%ld size=%dx%d vis=%d\n",
            (void*)h, r.left, r.top, r.right, r.bottom, w, ht, IsWindowVisible(h) ? 1 : 0);

    GdiplusStartupInput in;
    ULONG_PTR tok = 0;
    GdiplusStartup(&tok, &in, NULL);
    {
        HDC screen = GetDC(NULL);
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = cw;
        bi.bmiHeader.biHeight = -ch;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = NULL;
        HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        HGDIOBJ old = SelectObject(mem, dib);

        // fill with opaque magenta so untouched areas are obvious
        RECT all = {0, 0, cw, ch};
        HBRUSH mg = CreateSolidBrush(RGB(255, 0, 255));
        FillRect(mem, &all, mg);
        DeleteObject(mg);

        // PrintWindow renders the window into mem at its client origin
        BOOL ok = PrintWindow(h, mem, 2 /*PW_RENDERFULLCONTENT*/);
        DWORD err = GetLastError();
        wprintf(L"PrintWindow=%d err=%lu\n", ok, err);

        int nonMagenta = 0, opaque = 0;
        const BYTE* p = (const BYTE*)bits;
        for (int i = 0; i < cw * ch; ++i) {
            BYTE b = p[i * 4 + 0], gg = p[i * 4 + 1], rr = p[i * 4 + 2], aa = p[i * 4 + 3];
            if (!(rr == 255 && gg == 0 && b == 255 && aa == 255)) nonMagenta++;
            if (aa == 255) opaque++;
        }
        wprintf(L"pixels=%d nonMagenta=%d opaque=%d\n", cw * ch, nonMagenta, opaque);

        // save straight from the DIB while it is still selected in mem
        {
            Bitmap bmp(cw, ch, PixelFormat32bppARGB);
            Graphics g(&bmp);
            HDC dc = g.GetHDC();
            BitBlt(dc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
            g.ReleaseHDC(dc);
            CLSID png;
            if (GetEncoderClsid(L"image/png", &png)) {
                Status st = bmp.Save(outPath, &png, NULL);
                wprintf(L"save status=%d\n", (int)st);
            }
        }

        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
    }
    GdiplusShutdown(tok);
    wprintf(L"saved %ls\n", outPath);
    return 0;
}
