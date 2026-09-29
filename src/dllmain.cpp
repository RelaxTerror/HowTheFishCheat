// How to Fish - Internal Trainer (C++ DLL + ImGui + DX11 + Mono JIT patch)
// Derleme: x64, Release. Enjekte et: Injector.exe <pid> HowToFishInternal.dll
// Cikis: END tusu. Menu: INSERT.
#include <windows.h>
#include "renderer.h"
#include "gui.h"
#include "cheats.h"
#include "auth.h"
#include "mono_api.h"
#include "silentaim.h"
#include "casino.h"
#include "slot.h"
#include <tlhelp32.h>
#include <string>

static HMODULE g_self = nullptr;
static HANDLE g_thread = nullptr;
static HANDLE g_single = nullptr;
static volatile bool g_run = true;
static volatile bool g_duplicate = false;

static bool KeyPressed(int vk) { return (GetAsyncKeyState(vk) & 1) != 0; }

// Ayni process'te kac trainer DLL yuklu? (1'den fazlaysa eski hook'lar da aktif!)
static void AuditModules() {
    auto& st = State();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    int n = 0;
    std::string names;
    if (Module32FirstW(snap, &me)) do {
        std::wstring fn = me.szModule;
        for (auto& c : fn) c = towlower(c);
        if (fn.find(L"howtofishinternal") == std::wstring::npos) continue;
        ++n;
        char nb[128] = { 0 };
        size_t conv = 0;
        wcstombs_s(&conv, nb, me.szModule, _TRUNCATE);
        if (!names.empty()) names += ", ";
        names += nb;
    } while (Module32NextW(snap, &me));
    CloseHandle(snap);
    st.trainerCount = n;
    char buf[256];
    sprintf_s(buf, ">> Yuklu trainer: %d adet (%s)", n, names.c_str());
    st.Log(buf);
    if (n > 1)
        st.Log("[!] 1'DEN FAZLA TRAINER YUKLU! Eskiler de ates ediyor. Oyunu kapatip ac, SADECE yeni DLL enjekte et.");
}

static DWORD WINAPI MainThread(LPVOID) {
    if (g_duplicate) {
        MessageBoxW(nullptr,
            L"Eski trainer hala yuklu!\n\nOnce oyunda END tusuna bas (eski cikar),\nsonra yeni DLL'yi enjekte et.",
            L"HowToFish Trainer", MB_ICONWARNING | MB_OK);
        FreeLibraryAndExitThread(g_self, 0);
        return 0;
    }
    auto& st = State();
    st.Log("[*] Trainer v84 baslatildi.");
    auth::Init(g_self); // kayitli lisans varsa otomatik giris dene (worker)
    // Mono biraz gec yuklenebilir; 30 sn'ye kadar bekle
    for (int i = 0; i < 300 && g_run; ++i) {
        if (MonoAPI::Get().Attach()) break;
        Sleep(100);
    }
    st.monoReady = MonoAPI::Get().IsReady();
    st.status = st.monoReady ? "Mono bagli, hook kuruluyor..." : "Mono bulunamadi! Yine de menu acilacak.";
    st.Log(st.monoReady ? "[+] mono-2.0-bdwgc.dll baglandi." : "[!] Mono bulunamadi (dnSpy ile exe adini kontrol et).");

    if (st.monoReady) {
        // 2 sn bekle: Assembly-CSharp tam yuklensin, sonra tani dokumu bas
        Sleep(2000);
        MonoAPI::Get().Attach(); // thread保证
        cheats::DumpDiagnostics();
    }

    if (!renderer::Init()) {
        st.status = "DX11 hook kurulamadi!";
        st.Log("[!] renderer::Init basarisiz. Farkli render API (DX12/Vulkan) kullaniliyor olabilir.");
    } else {
        st.status = "Aktif. INSERT/HOME: menu | END: cikis";
    }

    if (st.monoReady) cheats::InstallUpdateHook(); // resmi para kuyrugu (yoksa ham yola dusulur)
    if (st.monoReady) silentaim::Install(); // silent aim Shoot hook
    if (st.monoReady) casino::Install(); // casino hook'lari

    AuditModules(); // kac trainer yuklu? (coklu yukleme teshisi)

    while (g_run) {
        if (KeyPressed(VK_INSERT) || KeyPressed(VK_HOME)) {
            renderer::ToggleMenuVisible(!renderer::IsMenuVisible());
            Sleep(200);
        }
        // Hotkey'ler yalnizca giris sonrasi calisir (INSERT/HOME menu, END cikis haric).
        if (auth::IsAuthed()) {
        if (KeyPressed(VK_F1)) { cheats::ToggleGod(); gui::Notify("Olumsuzluk", State().godMode); Sleep(200); }
        if (KeyPressed(VK_F2)) { cheats::ToggleHunger(); gui::Notify("Aclik kilidi", State().noHunger); Sleep(200); }
        if (KeyPressed(VK_F3)) { State().airJump = !State().airJump;
            gui::Notify("Infinite Jump", State().airJump);
            State().Log(State().airJump ? "[+] Infinite Jump acik" : "[-] Infinite Jump kapali"); Sleep(200); }
        if (KeyPressed(VK_F4)) { cheats::ToggleAmmo(); gui::Notify("No Reload", State().infAmmo); Sleep(200); }
        if (KeyPressed(VK_F5)) { cheats::CycleDamage(); gui::Notify(DmgLabel(State().dmgIndex), State().dmgIndex != 0); Sleep(200); }
        if (KeyPressed(VK_F6)) { cheats::AddMoney(State().moneyToAdd); Sleep(200); }
        if (KeyPressed(VK_F8)) { State().silentAim = !State().silentAim;
            if (!State().silentAim) State().espValid = false;
            gui::Notify("Silent Aim", State().silentAim);
            State().Log(State().silentAim ? "[+] Silent Aim acik" : "[-] Silent Aim kapali"); Sleep(200); }
        if (KeyPressed(VK_F9)) { State().casinoWin = !State().casinoWin;
            gui::Notify("Casino Always-Win", State().casinoWin);
            State().Log(State().casinoWin ? "[+] Casino Always-Win acik" : "[-] Casino kapali"); Sleep(200); }
        } // auth gate

        // Frame hook yoksa eski yol yalnizca yedek olarak kullanilir.
        if (!silentaim::HasFrameHook()) cheats::AirJumpTick();
        cheats::Tick(); // instance can/aclik tazeleme

        if (GetAsyncKeyState(VK_END) & 0x8000) break;
        Sleep(50); // Tick her 50ms yeterli, CPU yormasin
    }

    // Temizlik: TUM hileleri kapat, oyunu normale dondur, sonra cik
    cheats::ShutdownCheats();
    renderer::Shutdown();
    st.Log("[*] Cikis yapildi, DLL cozuluyor.");
    Sleep(300);
    FreeLibraryAndExitThread(g_self, 0);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        // Tekil koruma: eski surum icerideyse yukleme, MainThread uyarip cikar
        HANDLE m = CreateMutexW(nullptr, TRUE, L"HowToFishTrainer_Singleton");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (m) { ReleaseMutex(m); CloseHandle(m); }
            g_duplicate = true;
        } else {
            g_single = m; // sahip biziz, DETACH'te birakilir
        }
        g_run = true;
        g_thread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_run = false;
        if (g_single) { CloseHandle(g_single); g_single = nullptr; }
    }
    return TRUE;
}
