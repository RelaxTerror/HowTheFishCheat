#include "renderer.h"
#include "cheats.h"
#include "gui.h"
#include <d3d11.h>
#include <dxgi.h>
#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace renderer {
static HWND g_hwnd = nullptr;
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static ID3D11Texture2D* g_bb = nullptr; // su anki backbuffer (degisirse RTV yenilenir)
static WNDPROC g_origWnd = nullptr;
static bool g_inited = false;
static bool g_menu = true;
static bool g_rtvWarned = false;

typedef HRESULT(__stdcall* PresentFn)(IDXGISwapChain*, UINT, UINT);
static PresentFn oPresent = nullptr;

bool IsMenuVisible() { return g_menu; }
void ToggleMenuVisible(bool v) { g_menu = v; }

static LRESULT __stdcall HookWnd(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_menu && ImGui_ImplWin32_WndProcHandler(h, m, w, l))
        return 1;
    return CallWindowProcW(g_origWnd, h, m, w, l);
}

static void DropRTV() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_bb) { g_bb->Release(); g_bb = nullptr; }
}

static HRESULT __stdcall HookPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!g_inited) {
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_device))) {
            g_device->GetImmediateContext(&g_ctx);
            DXGI_SWAP_CHAIN_DESC d{};
            sc->GetDesc(&d);
            g_hwnd = d.OutputWindow;

            ImGui::CreateContext();
            ImGui::StyleColorsDark();
            ImGui_ImplWin32_Init(g_hwnd);
            ImGui_ImplDX11_Init(g_device, g_ctx);

            g_origWnd = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HookWnd);
            g_inited = true;
            State().Log("[+] DX11 hook kuruldu, INSERT veya HOME ile menu ac/kapa.");
        }
    }
    if (g_inited) {
        // Backbuffer degisti mi? (ada/sahne gecisi, cozunurluk, alt-tab)
        // Eski RTV ile cizmek GPU takilmasi / device-removed yapar. Her frame kontrol et.
        ID3D11Texture2D* cur = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&cur))) {
            if (cur != g_bb) {
                DropRTV();
                g_bb = cur; // referansi tut (Release etme)
                if (FAILED(g_device->CreateRenderTargetView(g_bb, nullptr, &g_rtv))) {
                    DropRTV();
                    if (!g_rtvWarned) {
                        g_rtvWarned = true;
                        State().Log("[!] RTV kurulamadi (D3D12 arka ucu olabilir), menu cizilmeyecek ama oyun calisir.");
                    }
                }
            } else {
                cur->Release();
            }
        }
        if (g_rtv) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            gui::DrawOverlay();
            if (g_menu) gui::Draw();
            ImGui::Render();
            g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    }
    HRESULT hr = oPresent(sc, sync, flags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        DropRTV(); // sonraki frame bastan kurulur
    return hr;
}

bool Init() {
    MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED) return false;

    // Dummy device ile Present vtable indexini bul (8)
    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, DefWindowProcW, 0, 0,
                    GetModuleHandleW(nullptr), nullptr, nullptr, nullptr, nullptr, L"HTF_Dummy", nullptr };
    RegisterClassExW(&wc);
    HWND hw = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hw; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    ID3D11Device* dev = nullptr; IDXGISwapChain* sc = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, nullptr);
    if (FAILED(hr)) { DestroyWindow(hw); UnregisterClassW(wc.lpszClassName, wc.hInstance); return false; }

    void** vt = *(void***)sc;
    void* presentAddr = vt[8];

    sc->Release(); dev->Release();
    DestroyWindow(hw); UnregisterClassW(wc.lpszClassName, wc.hInstance);

    if (MH_CreateHook(presentAddr, &HookPresent, (void**)&oPresent) != MH_OK) return false;
    if (MH_EnableHook(presentAddr) != MH_OK) return false;
    return true;
}

void Shutdown() {
    if (g_origWnd && g_hwnd) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_origWnd);
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    if (g_inited) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    if (g_rtv || g_bb) DropRTV();
}
} // namespace renderer
