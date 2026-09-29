// HowToFish Trainer - Tek-tikla Auto Injector
// Kullanim: Oyunu ac (ana menuye gel), sonra bu EXE'ye cift tikla. Hepsi bu.
// EXE + HowToFishInternal_v*.dll ayni klasorde dursun.
// Eski kullanim da calisir: HowToFishTrainer.exe "<pid veya exe adi>" <dll yolu>
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <string>
#include <iostream>
#include <vector>
#include <algorithm>

static void Logo() {
    std::wcout << L"============================================\n";
    std::wcout << L" How to Fish Trainer - Tek Tikla Inject\n";
    std::wcout << L"============================================\n\n";
}

static bool IsAdmin() {
    BOOL admin = FALSE;
    PSID group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(nullptr, group, &admin);
        FreeSid(group);
    }
    return admin == TRUE;
}

static void SelfElevate() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    ShellExecuteW(nullptr, L"runas", path, nullptr, nullptr, SW_SHOWNORMAL);
}

static DWORD FindPid(const std::wstring& name) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W p{};
    p.dwSize = sizeof(p);
    DWORD pid = 0;
    if (Process32FirstW(s, &p)) do {
        if (name == p.szExeFile) { pid = p.th32ProcessID; break; }
    } while (Process32NextW(s, &p));
    CloseHandle(s);
    return pid;
}

// EXE'nin bulundugu klasorde HowToFishInternal*.dll ara, en yenisini sec
// (v80, v81... surum kilidini asmak icin surum sayisi artiyor)
static std::wstring FindDllNextToExe() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    size_t bs = dir.find_last_of(L"\\/");
    dir = (bs == std::wstring::npos) ? L"." : dir.substr(0, bs);

    std::wstring pattern = dir + L"\\HowToFishInternal*.dll";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";

    std::vector<std::wstring> found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            found.push_back(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (found.empty()) return L"";
    std::sort(found.begin(), found.end()); // v79 < v80 < v81...
    return found.back(); // en yeni
}

static bool Inject(DWORD pid, const std::wstring& dllPath) {
    HANDLE h = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!h) {
        std::wcout << L"[!] OpenProcess basarisiz (" << GetLastError()
                   << L"). EXE'yi sag tik > Yonetici olarak calistir.\n";
        return false;
    }
    size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID mem = VirtualAllocEx(h, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { std::wcout << L"[!] VirtualAllocEx basarisiz.\n"; CloseHandle(h); return false; }
    if (!WriteProcessMemory(h, mem, dllPath.c_str(), bytes, nullptr)) {
        std::wcout << L"[!] WriteProcessMemory basarisiz.\n";
        VirtualFreeEx(h, mem, 0, MEM_RELEASE); CloseHandle(h); return false;
    }
    HANDLE t = CreateRemoteThread(h, nullptr, 0,
        (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"),
        mem, 0, nullptr);
    if (!t) {
        std::wcout << L"[!] CreateRemoteThread basarisiz (" << GetLastError() << L").\n";
        VirtualFreeEx(h, mem, 0, MEM_RELEASE); CloseHandle(h); return false;
    }
    WaitForSingleObject(t, INFINITE);
    DWORD mod = 0;
    GetExitCodeThread(t, &mod);
    VirtualFreeEx(h, mem, 0, MEM_RELEASE);
    CloseHandle(t); CloseHandle(h);
    return mod != 0;
}

static const wchar_t* kTargets[] = {
    L"How to Fish.exe",
    L"HowToFish.exe",
    L"How To Fish.exe",
};

int wmain(int argc, wchar_t** argv) {
    SetConsoleTitleW(L"How to Fish Trainer");
    Logo();

    // --- Klasik kullanim (geri uyumluluk): EXE <pid|prosesAdi> <dllYolu> ---
    if (argc >= 3) {
        DWORD pid = 0;
        try { pid = (DWORD)_wtoi(argv[1]); } catch (...) {}
        if (pid == 0) pid = FindPid(argv[1]);
        if (pid == 0) { std::wcout << L"[!] Proses bulunamadi.\n"; system("pause"); return 2; }
        std::wcout << L"[*] PID " << pid << L" -> " << argv[2] << L"\n";
        bool ok = Inject(pid, argv[2]);
        std::wcout << (ok ? L"\n[+] Enjekte edildi! Oyuna don, INSERT ile menuyu ac.\n"
                          : L"\n[!] LoadLibrary basarisiz.\n");
        system("pause");
        return ok ? 0 : 5;
    }

    // --- Admin kontrolu: degilse kendini admin olarak yeniden baslat ---
    if (!IsAdmin()) {
        std::wcout << L"[*] Admin yetkisi isteniyor...\n";
        SelfElevate();
        return 0;
    }

    // --- DLL'yi yan klasorde otomatik bul ---
    std::wstring dll = FindDllNextToExe();
    if (dll.empty()) {
        std::wcout << L"[!] DLL bulunamadi!\n";
        std::wcout << L"    HowToFishInternal_v*.dll dosyasi bu EXE'nin yaninda olmali.\n";
        std::wcout << L"    build\\Release klasorundeki 2 dosyayi ayni yere koy:\n";
        std::wcout << L"      - HowToFishTrainer.exe\n      - HowToFishInternal_v*.dll\n";
        system("pause");
        return 1;
    }
    std::wcout << L"[*] DLL: " << dll << L"\n";

    // --- Oyunu bekle (60 sn): kullanici EXE'ye cift tiklar, oyun aciksa aninda bulur ---
    DWORD pid = 0;
    std::wstring foundName;
    for (int i = 0; i < 60 && pid == 0; ++i) {
        for (auto* n : kTargets) {
            pid = FindPid(n);
            if (pid) { foundName = n; break; }
        }
        if (pid) break;
        if (i == 0)
            std::wcout << L"[*] Oyun araniyor (How to Fish.exe)... oyunu ac, ana menuye gel.\n";
        Sleep(1000);
    }
    if (!pid) {
        std::wcout << L"\n[!] Oyun bulunamadi. Once oyunu baslatip ana menuye gel, sonra EXE'yi ac.\n";
        system("pause");
        return 2;
    }
    std::wcout << L"[+] Oyun bulundu: " << foundName << L" (PID " << pid << L")\n";
    std::wcout << L"[*] Enjekte ediliyor...\n";
    Sleep(500);

    if (Inject(pid, dll)) {
        std::wcout << L"\n============================================\n";
        std::wcout << L" [+] 10 NUMARA! Inject basarili.\n";
        std::wcout << L" Oyuna don -> INSERT ile menuyu ac.\n";
        std::wcout << L" Cikis: oyunda END tusuna bas.\n";
        std::wcout << L" Bu pencereyi kapatabilirsin.\n";
        std::wcout << L"============================================\n";
    } else {
        std::wcout << L"\n[!] Enjekte edilemedi.\n";
        std::wcout << L" - Oyunun 64-bit oldugundan emin ol\n";
        std::wcout << L" - Antivirus DLL'yi silmis/karantinaya almis olabilir\n";
        std::wcout << L" - Oyunda eski trainer yukluyse once END ile cikar, oyunu kapatip ac\n";
    }
    system("pause");
    return 0;
}
