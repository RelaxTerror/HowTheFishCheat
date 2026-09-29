#pragma once
// Hile durumlari + ImGui log tamponu. Renderer/GUI ile paylasilir.
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

struct TrainerState {
    bool showMenu = true;

    bool godMode   = false; // F1: can kilidi
    bool noHunger  = false; // F2: aclik kilidi
    bool noRecoil  = false; // Shoot hook: spread + kick + model recoil sifir
    bool airJump   = false; // F3: sonsuz ziplama (hizli ziplama spam)
    bool infAmmo   = false; // F4: sinirsiz mermi
    int  dmgIndex  = 0;     // F5: 0=1x 1=2x 2=5x 3=10x 4=OneShot
    int  moneyToAdd = 10000;

    float speedMult = 1.0f;
    bool  speedHack = false;

    // silent aim (F8)
    bool  silentAim = false;
    float silentFov = 15.0f;
    float silentMaxDist = 250.0f; // metre, otesi hedef sayilmaz (uzaktaki kuslar icin yuksek)
    float silentBulletSpeed = 0.0f; // m/s; 0=lead kapali (hitscan). Hizli hedefte yukselt (or. 60-150)
    int   silentShots = 0; // yonlendirilen atis sayaci (tani icin)
    // ESP (silent aim hedef gostergesi)
    bool  espEnabled = true;
    bool  espShowFov = false;
    bool  espShowTracer = false;
    bool  espShowReticle = false;
    bool  espShowEntityBox = true;
    bool  espShowLabel = true;
    bool  hudEnabled = true;
    bool  hudActiveList = true;
    bool  espValid = false;   // gecerli hedef var mi?
    float espTx = 0, espTy = 0, espTz = 0; // hedef dunya konumu
    float espEx = .5f, espEy = .5f, espEz = .5f; // collider extents
    int   espTargetKind = 0; // 1=Fish, 2=Bird, 3=Albatross
    float espCx = 0, espCy = 0, espCz = 0; // atis anindaki kamera konumu
    float espFx = 0, espFy = 0, espFz = 1; // atis anindaki kamera ileri
    float espFovY = 60.f;     // kamera dikey FOV
    float espDist = 0.f;      // hedef mesafesi (m)

    // casino (F9)
    bool  casinoWin = false;
    float casinoMult = 1.0f;
    int   casinoColor = 1; // 0=Black 1=Red 2=Green (BetColor enum sirasi)

    int trainerCount = 1; // yuklu trainer DLL sayisi (1'den fazlaysa eski surum icerde)

    // para kilidi (F6 otomatik acar, Tick hedefte tutar)
    bool moneyLock = false;
    int  moneyLockValue = 0;

    // instance cache (orijinal degerleri geri yazmak icin, ham 4 byte)
    uint32_t baseWalkRaw = 0, baseSprintRaw = 0, basePunchRaw = 0, baseProjRaw = 0;
    bool speedCached = false, dmgCached = false, projCached = false;

    bool monoReady = false;
    std::string status = "Baslatiliyor...";

    std::vector<std::string> log;
    std::mutex logMtx;
    void Log(const std::string& s) {
        std::lock_guard<std::mutex> l(logMtx);
        log.push_back(s);
        if (log.size() > 200) log.erase(log.begin());
    }
};

TrainerState& State();
const char* DmgLabel(int i);

namespace cheats {
bool ApplyGod(bool on);
bool ApplyHunger(bool on);
void SetNoRecoil(bool on); // native Weapon.Shoot/AddModelRecoil hook'larini kontrol eder
bool ApplyAmmo(bool on);
void ToggleGod();
void ToggleHunger();
void ToggleAmmo();
void CycleDamage();
bool AddMoney(int amount);
bool InstallUpdateHook(); // MoneyManager.Update hook: resmi AddMoney kuyrugu (main thread)
bool LogMoney(); // sadece okur, yan etkisiz tani
void AirJumpTick();
void NoReloadFrameTick(); // native Player.Update icinde ammo/reload uygular
// Her frame cagrilir: instance-based can/aclik/mermi tazeleme
void Tick();
// Mono baglaninca bir kere cagrilir: ilgili siniflarin metod+field listesini loga basar
void DumpDiagnostics();
// END cikisi: tum bayraklari kapat + yazilan degerleri geri al (~1 sn).
// Sonrasi oyun hilesiz devam eder (para miktari geri alinmaz, hile kapali kalir).
void ShutdownCheats();
} // namespace cheats
