// ozbench_control_window - test-only subject for the external_visible_frame
// collector. It shows a blank (solid) client area first, then a photo-like
// pattern (multi-level checkerboard plus a moving bar) so tests can prove the
// probe rejects the blank and matches the pattern. The moving bar keeps the
// desktop composing frames while the pattern is shown.
//
// Flags:
//   --title TEXT        window title (Unicode-safe)
//   --blank-ms N        show the blank for N ms before the pattern (default 0)
//   --pattern           show the pattern immediately
//   --size WxH          client size (default 640x480)
//   --color R,G,B       blank color (default 32,32,32)
//   --animate-ms N      redraw interval while the pattern is shown (default 50;
//                       0 disables animation)
//   --exit-after-ms N   auto-exit after N ms (default 0 = stay until WM_CLOSE)
//   --ignore-close      ignore WM_CLOSE (forces Job cleanup)
//   --no-window         create no window and sleep (missing-window tests)
//   --topmost           keep the window above other windows (probe tests)
//   --image-path PATH   accepted and ignored (mirrors real subject CLI)
#include <windows.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct app_state {
    bool pattern = false;
    bool ignore_close = false;
    bool no_window = false;
    bool topmost = false;
    int animate_ms = 50;
    int anim_x = 0;
    COLORREF blank_color = RGB(32, 32, 32);
    int width = 640;
    int height = 480;
    UINT_PTR blank_timer = 0;
    UINT_PTR anim_timer = 0;
    std::uint64_t exit_after_ms = 0;
    std::uint64_t start_tick = 0;
};

void draw_pattern(HDC hdc, const app_state& s) {
    const int cell = 8;
    for (int y = 0; y < s.height; y += cell) {
        for (int x = 0; x < s.width; x += cell) {
            const int level = ((x / cell) + (y / cell)) % 4;
            const int v = 40 + level * 55;  // 40, 95, 150, 205
            HBRUSH b = CreateSolidBrush(RGB(v, v, v));
            RECT r{x, y, std::min(x + cell, s.width), std::min(y + cell, s.height)};
            FillRect(hdc, &r, b);
            DeleteObject(b);
        }
    }
    // Moving bar: guarantees the desktop keeps composing frames.
    RECT bar{s.anim_x, 0, std::min(s.anim_x + 4, s.width), s.height};
    HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(hdc, &bar, wb);
    DeleteObject(wb);
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<app_state*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            if (s && !s->pattern) {
                HBRUSH b = CreateSolidBrush(s->blank_color);
                FillRect(hdc, &ps.rcPaint, b);
                DeleteObject(b);
            } else if (s) {
                draw_pattern(hdc, *s);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_TIMER:
            if (wp == s->blank_timer) {
                KillTimer(hwnd, s->blank_timer);
                s->blank_timer = 0;
                s->pattern = true;
                if (s->animate_ms > 0) {
                    s->anim_timer = SetTimer(hwnd, 2, static_cast<UINT>(s->animate_ms),
                                             nullptr);
                }
                InvalidateRect(hwnd, nullptr, TRUE);
            } else if (wp == s->anim_timer) {
                s->anim_x = (s->anim_x + 8) % std::max(1, s->width);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        case WM_CLOSE:
            if (s && s->ignore_close) {
                return 0;  // refuse to close; Job cleanup must force it
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    app_state s;
    std::wstring title = L"ozbench control window";
    long blank_ms = 0;
    bool pattern = false;

    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--title" && i + 1 < argc) {
            title = argv[++i];
        } else if (a == L"--blank-ms" && i + 1 < argc) {
            blank_ms = _wtoi(argv[++i]);
        } else if (a == L"--pattern") {
            pattern = true;
        } else if (a == L"--size" && i + 1 < argc) {
            const std::wstring sz = argv[++i];
            const auto x = sz.find(L'x');
            if (x != std::wstring::npos) {
                s.width = _wtoi(sz.substr(0, x).c_str());
                s.height = _wtoi(sz.substr(x + 1).c_str());
            }
        } else if (a == L"--color" && i + 1 < argc) {
            const std::wstring c = argv[++i];
            int r = 32, g = 32, b = 32;
            std::swscanf(c.c_str(), L"%d,%d,%d", &r, &g, &b);
            s.blank_color = RGB(r, g, b);
        } else if (a == L"--animate-ms" && i + 1 < argc) {
            s.animate_ms = _wtoi(argv[++i]);
        } else if (a == L"--exit-after-ms" && i + 1 < argc) {
            s.exit_after_ms = static_cast<std::uint64_t>(_wtoi(argv[++i]));
        } else if (a == L"--ignore-close") {
            s.ignore_close = true;
        } else if (a == L"--no-window") {
            s.no_window = true;
        } else if (a == L"--topmost") {
            s.topmost = true;
        } else if (a == L"--image-path" && i + 1 < argc) {
            ++i;  // accepted and ignored
        } else {
            std::fwprintf(stderr, L"ozbench_control_window: unknown argument %s\n",
                          a.c_str());
            return 2;
        }
    }
    if (s.width < 16 || s.height < 16) {
        s.width = 640;
        s.height = 480;
    }

    if (s.no_window) {
        Sleep(30000);
        return 0;
    }

    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = L"OZBenchControlWindow";
    if (!RegisterClassW(&wc)) {
        return 3;
    }

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(),
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                                CW_USEDEFAULT, s.width + 16, s.height + 39, nullptr,
                                nullptr, inst, nullptr);
    if (!hwnd) {
        return 4;
    }
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    if (s.topmost) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
    s.start_tick = GetTickCount64();
    if (pattern) {
        s.pattern = true;
        if (s.animate_ms > 0) {
            s.anim_timer = SetTimer(hwnd, 2, static_cast<UINT>(s.animate_ms), nullptr);
        }
    } else if (blank_ms > 0) {
        s.blank_timer = SetTimer(hwnd, 1, static_cast<UINT>(blank_ms), nullptr);
    }

    MSG msg{};
    while (true) {
        if (s.exit_after_ms > 0 &&
            GetTickCount64() - s.start_tick >= s.exit_after_ms) {
            DestroyWindow(hwnd);
        }
        const BOOL got = PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);
        if (got) {
            if (msg.message == WM_QUIT) {
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        } else {
            Sleep(5);
        }
    }
    return 0;
}