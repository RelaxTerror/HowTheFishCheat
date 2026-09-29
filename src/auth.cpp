// emirakar.com lisans dogrulama (WinHTTP, senkron + worker thread).
// API: POST /api/client/verify/ {app_id, secret, version, license_key, hwid}
//      POST /api/client/announcements/ {app_id, secret}
#include "auth.h"
#include <windows.h>
#include <winhttp.h>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cstdint>
#pragma comment(lib, "winhttp.lib")

namespace auth {
namespace {

const char* kHost = "emirakar.com";
const char* kAppId = "app_dbe9394cea412e8c02";
const char* kVersion = "1.0";
// secret XOR 0x5A ile gomulu (duz metin taramasina takilmamak icin).
const unsigned char kSecX[] = {
    63, 59, 41, 5, 109, 104, 41, 11, 104, 12, 98, 108, 27, 49, 10, 34,
    57, 25, 99, 24, 98, 61, 105, 5, 35, 2, 49, 12, 23, 21, 109, 34,
    10, 43, 29, 62
};
const wchar_t* kKeyFile = L"htf_license.dat";

std::mutex g_mtx;
bool g_authed = false;
bool g_working = false;
std::string g_status = "Giris yapilmadi.";
LicenseInfo g_info;
std::vector<Notice> g_notices;
std::string g_hwid;
wchar_t g_keyPath[MAX_PATH] = { 0 };

std::string DecodeSecret() {
    std::string s;
    s.resize(sizeof(kSecX));
    for (size_t i = 0; i < sizeof(kSecX); ++i) s[i] = (char)(kSecX[i] ^ 0x5A);
    return s;
}

static uint64_t Fnv1a(const char* s) {
    uint64_t h = 14695981039346656037ULL;
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ULL; }
    return h;
}

std::string MakeHwid() {
    std::lock_guard<std::mutex> l(g_mtx);
    if (!g_hwid.empty()) return g_hwid;
    char base[192] = { 0 };
    wchar_t w[128] = { 0 };
    DWORD cb = sizeof(w);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography",
                     L"MachineGuid", RRF_RT_REG_SZ, nullptr, w, &cb) == ERROR_SUCCESS) {
        WideCharToMultiByte(CP_UTF8, 0, w, -1, base, sizeof(base) - 1, nullptr, nullptr);
    } else {
        char comp[64] = { 0 }, user[64] = { 0 };
        DWORD n1 = sizeof(comp), n2 = sizeof(user);
        GetComputerNameA(comp, &n1);
        GetUserNameA(user, &n2);
        snprintf(base, sizeof(base), "%s-%s", comp, user);
    }
    char out[32];
    snprintf(out, sizeof(out), "HTF-%016llX", (unsigned long long)Fnv1a(base));
    g_hwid = out;
    return g_hwid;
}

// Minimal JSON: "key": <deger> bulur. Obje ici arama icin start verilebilir.
const char* SkipWs(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
    return p;
}
const char* FindKey(const char* j, const char* key, const char* start = nullptr) {
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* from = start ? start : j;
    const char* f = strstr(from, pat);
    if (!f) return nullptr;
    const char* c = SkipWs(f + strlen(pat));
    if (*c != ':') return nullptr;
    return SkipWs(c + 1);
}
void AppendUtf8(std::string& o, unsigned cp) {
    if (cp < 0x80) o += (char)cp;
    else if (cp < 0x800) {
        o += (char)(0xC0 | (cp >> 6));
        o += (char)(0x80 | (cp & 0x3F));
    } else {
        o += (char)(0xE0 | (cp >> 12));
        o += (char)(0x80 | ((cp >> 6) & 0x3F));
        o += (char)(0x80 | (cp & 0x3F));
    }
}
bool JsonStr(const char* j, const char* key, std::string& out, const char* start = nullptr) {
    const char* v = FindKey(j, key, start);
    if (!v || *v != '"') return false;
    ++v;
    out.clear();
    while (*v && *v != '"') {
        if (*v == '\\') {
            ++v;
            if (*v == 'n') { out += '\n'; ++v; }
            else if (*v == 't') { out += '\t'; ++v; }
            else if (*v == 'r') { out += '\r'; ++v; }
            else if (*v == 'u' && isxdigit((unsigned char)v[1])) {
                char hex[5] = { v[1], v[2], v[3], v[4], 0 };
                AppendUtf8(out, (unsigned)strtoul(hex, nullptr, 16));
                v += 5;
            }
            else if (*v) { out += *v++; }
        } else out += *v++;
    }
    return true;
}
bool JsonBool(const char* j, const char* key, bool& out, const char* start = nullptr) {
    const char* v = FindKey(j, key, start);
    if (!v) return false;
    if (strncmp(v, "true", 4) == 0) { out = true; return true; }
    if (strncmp(v, "false", 5) == 0) { out = false; return true; }
    return false;
}
bool JsonInt(const char* j, const char* key, long& out, const char* start = nullptr) {
    const char* v = FindKey(j, key, start);
    if (!v || (!(*v >= '0' && *v <= '9') && *v != '-')) return false;
    out = strtol(v, nullptr, 10);
    return true;
}

bool HttpPost(const char* path, const std::string& body, long& code, std::string& resp) {
    code = 0;
    resp.clear();
    wchar_t wpath[128] = { 0 };
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 127);
    HINTERNET hS = WinHttpOpen(L"HTFTrainer/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return false;
    WinHttpSetTimeouts(hS, 10000, 10000, 10000, 10000);
    HINTERNET hC = WinHttpConnect(hS, L"emirakar.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hC) { WinHttpCloseHandle(hS); return false; }
    HINTERNET hR = WinHttpOpenRequest(hC, L"POST", wpath, nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hR) {
        WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
        return false;
    }
    const wchar_t* hdrs = L"Content-Type: application/json\r\n";
    BOOL ok = WinHttpSendRequest(hR, hdrs, (DWORD)-1L,
                                 (LPVOID)body.data(), (DWORD)body.size(),
                                 (DWORD)body.size(), 0);
    if (ok) ok = WinHttpReceiveResponse(hR, nullptr);
    if (ok) {
        DWORD n = sizeof(code);
        WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            nullptr, &code, &n, nullptr);
        char buf[4096];
        DWORD rd = 0;
        while (WinHttpReadData(hR, buf, sizeof(buf), &rd) && rd > 0)
            resp.append(buf, rd);
    }
    WinHttpCloseHandle(hR);
    WinHttpCloseHandle(hC);
    WinHttpCloseHandle(hS);
    return ok == TRUE;
}

void SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> l(g_mtx);
    g_status = s;
}

bool Verify(const char* key, const char* hwid, std::string& msg) {
    char body[1024];
    snprintf(body, sizeof(body),
             "{\"app_id\":\"%s\",\"secret\":\"%s\",\"version\":\"%s\","
             "\"license_key\":\"%s\",\"hwid\":\"%s\"}",
             kAppId, DecodeSecret().c_str(), kVersion, key, hwid);
    long code = 0;
    std::string resp;
    if (!HttpPost("/api/client/verify/", body, code, resp)) {
        msg = "Sunucuya ulasilamadi (baglanti hatasi).";
        return false;
    }
    const char* j = resp.c_str();
    bool suc = false;
    JsonBool(j, "success", suc);
    std::string m;
    if (!JsonStr(j, "message", m)) m = "Bilinmeyen yanit.";
    if (code == 426) { msg = m.empty() ? "Surum eslesmiyor, guncelleyin." : m; return false; }
    if (!suc || (code != 200 && code != 0)) { msg = m; return false; }
    const char* lic = strstr(j, "\"license\"");
    LicenseInfo info;
    if (lic) {
        JsonStr(j, "username", info.username, lic);
        JsonStr(j, "level", info.level, lic);
        JsonStr(j, "status", info.status, lic);
        JsonStr(j, "expiresAt", info.expiresAt, lic);
        long dl = 0;
        if (JsonInt(j, "days_left", dl, lic)) info.days_left = (int)dl;
    }
    if (info.username.empty()) info.username = "-";
    {
        std::lock_guard<std::mutex> l(g_mtx);
        g_info = info;
    }
    msg = m;
    return true;
}

void FetchNotices() {
    std::vector<Notice> out;
    char body[512];
    snprintf(body, sizeof(body), "{\"app_id\":\"%s\",\"secret\":\"%s\"}",
             kAppId, DecodeSecret().c_str());
    long code = 0;
    std::string resp;
    if (!HttpPost("/api/client/announcements/", body, code, resp)) return;
    const char* j = resp.c_str();
    const char* arr = strchr(j, '[');
    if (!arr) return;
    // Seviye-1 objeleri tara, baslik/govde benzeri alanlari cek
    const char* p = arr;
    int depth = 0;
    const char* objStart = nullptr;
    for (; *p; ++p) {
        if (*p == '{') {
            if (depth == 1) objStart = p;
            ++depth;
        } else if (*p == '}') {
            --depth;
            if (depth == 1 && objStart) {
                std::string chunk(objStart, p + 1);
                Notice ntc;
                const char* cj = chunk.c_str();
                if (!JsonStr(cj, "title", ntc.title))
                    JsonStr(cj, "baslik", ntc.title);
                if (!JsonStr(cj, "content", ntc.body))
                    if (!JsonStr(cj, "body", ntc.body))
                        if (!JsonStr(cj, "text", ntc.body))
                            if (!JsonStr(cj, "message", ntc.body))
                                JsonStr(cj, "description", ntc.body);
                if (!ntc.title.empty() || !ntc.body.empty()) {
                    if (ntc.title.empty()) ntc.title = "Duyuru";
                    out.push_back(ntc);
                    if (out.size() >= 10) break;
                }
                objStart = nullptr;
            }
            if (depth <= 0) break;
        }
    }
    std::lock_guard<std::mutex> l(g_mtx);
    g_notices = out;
}

void SaveKey(const char* key) {
    if (!g_keyPath[0]) return;
    HANDLE h = CreateFileW(g_keyPath, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, key, (DWORD)strlen(key), &w, nullptr);
    CloseHandle(h);
}

DWORD WINAPI Worker(LPVOID p) {
    char* key = (char*)p;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        g_working = true;
        g_status = "Dogrulaniyor...";
    }
    std::string hwid = MakeHwid();
    std::string msg;
    bool ok = Verify(key, hwid.c_str(), msg);
    {
        std::lock_guard<std::mutex> l(g_mtx);
        g_authed = ok;
        g_working = false;
        g_status = ok ? ("Giris basarili. " + msg) : ("Giris basarisiz: " + msg);
        if (!ok) { g_info = LicenseInfo(); g_notices.clear(); }
    }
    if (ok) {
        SaveKey(key);
        FetchNotices();
    }
    delete[] key;
    return 0;
}

DWORD WINAPI AutoWorker(LPVOID) {
    if (!g_keyPath[0]) return 0;
    HANDLE h = CreateFileW(g_keyPath, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char key[64] = { 0 };
    DWORD r = 0;
    ReadFile(h, key, sizeof(key) - 1, &r, nullptr);
    CloseHandle(h);
    if (r == 0) return 0;
    // gecerli karakterleri birak
    std::string clean;
    for (char c : std::string(key)) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-') {
            clean += c;
            if (clean.size() >= 63) break;
        }
    }
    if (clean.empty()) return 0;
    {
        std::lock_guard<std::mutex> l(g_mtx);
        g_status = "Kayitli anahtar dogrulaniyor...";
    }
    char* cp = new char[clean.size() + 1];
    memcpy(cp, clean.c_str(), clean.size() + 1);
    Worker(cp);
    return 0;
}

} // namespace

void Init(void* dllModule) {
    wchar_t path[MAX_PATH] = { 0 };
    GetModuleFileNameW((HMODULE)dllModule, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) {
        wcsncpy_s(g_keyPath, path, (slash - path) + 1);
        wcscat_s(g_keyPath, kKeyFile);
    }
    HANDLE h = CreateThread(nullptr, 0, AutoWorker, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
}

bool IsAuthed() {
    std::lock_guard<std::mutex> l(g_mtx);
    return g_authed;
}
bool IsWorking() {
    std::lock_guard<std::mutex> l(g_mtx);
    return g_working;
}
void LoginAsync(const char* key) {
    {
        std::lock_guard<std::mutex> l(g_mtx);
        if (g_working) return;
    }
    if (!key || !*key) {
        SetStatus("Lisans anahtari bos.");
        return;
    }
    std::string clean;
    for (const char* p = key; *p && clean.size() < 63; ++p) {
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-')
            clean += c;
    }
    if (clean.empty()) {
        SetStatus("Gecersiz anahtar formati.");
        return;
    }
    char* cp = new char[clean.size() + 1];
    memcpy(cp, clean.c_str(), clean.size() + 1);
    HANDLE h = CreateThread(nullptr, 0, Worker, cp, 0, nullptr);
    if (h) CloseHandle(h);
    else delete[] cp;
}
void Logout() {
    std::lock_guard<std::mutex> l(g_mtx);
    g_authed = false;
    g_info = LicenseInfo();
    g_notices.clear();
    g_status = "Cikis yapildi.";
    if (g_keyPath[0]) DeleteFileW(g_keyPath);
}
std::string Status() {
    std::lock_guard<std::mutex> l(g_mtx);
    return g_status;
}
LicenseInfo Info() {
    std::lock_guard<std::mutex> l(g_mtx);
    return g_info;
}
std::vector<Notice> Notices() {
    std::lock_guard<std::mutex> l(g_mtx);
    return g_notices;
}
std::string Hwid() {
    return MakeHwid();
}

} // namespace auth
