#pragma once
// Lisans girisi (emirakar.com client API). Giris yapilmadan menu ozellikleri
// ve hotkey'ler kilitlidir.
#include <string>
#include <vector>

namespace auth {
struct LicenseInfo {
    std::string username = "-";
    std::string level = "-";
    std::string status = "-";
    std::string expiresAt = "-";
    int days_left = 0;
};
struct Notice {
    std::string title;
    std::string body;
};

void Init(void* dllModule);       // kayitli anahtar varsa otomatik giris dener (worker)
bool IsAuthed();
bool IsWorking();
void LoginAsync(const char* key); // dogrulamayi worker thread'de yapar
void Logout();
std::string Status();             // son durum mesaji
LicenseInfo Info();
std::vector<Notice> Notices();
std::string Hwid();               // kararli cihaz kimligi (<=256 char)
} // namespace auth
