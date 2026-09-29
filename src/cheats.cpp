#include "cheats.h"
#include "mono_api.h"
#include "memory.h"
#include "silentaim.h"
#include "fishspawn.h"
#include "slot.h"
#include "MinHook.h"
#include <windows.h>
#include <stdlib.h>
#include <cstring>
#include <cstdio>

TrainerState& State() { static TrainerState s; return s; }
const char* DmgLabel(int i) {
    switch (i) {
        case 1: return "2x"; case 2: return "5x";
        case 3: return "10x"; case 4: return "One-Shot";
        default: return "1x";
    }
}

// HOTFIX: Vitals JIT patch kapali. TakeDamage/LocalHit/DamageFromFullness/ApplyNewFire/
// ApplyNewPoison/LowerFullness'a RET yazmak FishNet RPC akisini bozuyordu.
// Can/aclik artik SADECE Tick icindeki instance yazmayla calisiyor (kod degisikligi yok).
// JIT sadece Weapon.set_Ammo icin kullaniliyor (void setter, risksiz).
struct JitTarget { const char* klass; const char* method; mem::JitPatch patch; };

static JitTarget kAmmoTargets[] = {
    { "Weapon", "set_Ammo", {} },
};

static void DumpClassFull(const char* klassName) {
    auto& st = State();
    auto& mono = MonoAPI::Get();
    MonoClass* c = mono.FindClass("", klassName);
    if (!c) { st.Log(std::string(">> ") + klassName + " SINIF BULUNAMADI"); return; }
    std::string lm = std::string(">> ") + klassName + " METOD: ";
    int n = 0;
    mono.ForEachMethod(c, [&](MonoMethod* m) {
        if (n++ < 80) { lm += mono.MethodName(m); lm += ", "; }
    });
    st.Log(lm);
    std::string lf = std::string(">> ") + klassName + " FIELD: ";
    mono.ForEachField(c, [&](MonoClassField* f) {
        lf += mono.FieldName(f); lf += ", ";
        if (lf.size() > 600) return;
    });
    st.Log(lf);
}

static bool PatchList(JitTarget* list, size_t n, bool enable) {
    auto& st = State();
    auto& mono = MonoAPI::Get();
    bool allOk = true;
    for (size_t i = 0; i < n; ++i) {
        if (enable && !list[i].patch.active()) {
            MonoClass* c = mono.FindClass("", list[i].klass);
            if (!c) { st.Log(std::string("Sinif bulunamadi: ") + list[i].klass); allOk = false; continue; }
            MonoMethod* m = mono.FindMethod(c, list[i].method);
            if (!m) m = mono.FindMethodAny(c, list[i].method);
            if (!m) {
                st.Log(std::string("Metod bulunamadi: ") + list[i].klass + "." + list[i].method);
                DumpClassFull(list[i].klass);
                allOk = false; continue;
            }
            void* code = mono.CompileMethod(m);
            if (!code) {
                st.Log(std::string("JIT alinamadi, instance yola dusuluyor: ") + list[i].method);
                DumpClassFull(list[i].klass);
                allOk = false; continue;
            }
            if (!list[i].patch.apply(code, 1))
                { st.Log(std::string("Patch basarisiz: ") + list[i].method); allOk = false; }
            else
                st.Log(std::string("[+] Patched: ") + list[i].klass + "." + list[i].method);
        } else if (!enable && list[i].patch.active()) {
            list[i].patch.restore();
            st.Log(std::string("[-] Restored: ") + list[i].klass + "." + list[i].method);
        }
    }
    return allOk;
}

namespace cheats {

bool ApplyGod(bool on)    { auto& s = State(); s.godMode = on;
    s.Log(on ? "[+] Olumsuzluk acik (instance, Tick)" : "[-] Olumsuzluk kapali"); return true; }
bool ApplyHunger(bool on) { auto& s = State(); s.noHunger = on;
    s.Log(on ? "[+] Aclik kilidi acik (instance, Tick)" : "[-] Aclik kilidi kapali"); return true; }

void SetNoRecoil(bool on) {
    auto& s = State();
    s.noRecoil = on;
    s.Log(on ? "[+] No Recoil acik (Weapon + BarrelAttachment instance)"
             : "[-] No Recoil kapali; orijinal degerler geri yuklenecek.");
}
bool ApplyAmmo(bool on)   { return PatchList(kAmmoTargets, sizeof(kAmmoTargets)/sizeof(kAmmoTargets[0]), on); }

void ToggleGod()    { auto& s = State(); s.godMode  = !s.godMode;  ApplyGod(s.godMode); }
void ToggleHunger() { auto& s = State(); s.noHunger = !s.noHunger; ApplyHunger(s.noHunger); }
void ToggleAmmo()   { auto& s = State(); s.infAmmo  = !s.infAmmo;  ApplyAmmo(s.infAmmo); }
void CycleDamage()  {
    auto& s = State();
    s.dmgIndex = (s.dmgIndex + 1) % 5;
    s.Log(std::string("Hasar carpani: ") + DmgLabel(s.dmgIndex));
    // Not: hasar carpani icin PlayerPunching._damage / Melee / Attachments / WeaponInfo
    // instance degerleri her silah spawn'inda tazelenir. Tam surum icin dnSpy'dan
    // field offsetlerini bulup her frame yazan loop gerekir (asagidaki TODO).
    // Su an menu degeri saklanir; instance writer eklendiginde buraya baglanir.
}

// ---------- Resmi para kuyrugu (main-thread) ----------
// AddMoney(int, Player) oyunun kendi fonksiyonu: sync + hook + UI + ses hepsi resmi.
// Ama SADECE main thread'de cagrilmali (arka plandan crash). Cozum:
// MoneyManager.Update oyunun main thread'inde her frame calisir -> hook'la,
// kuyruktaki miktari orada islet. F6 sadece kuyruga yazar (16ms icinde uygulanir).
typedef void (__cdecl* UpdateFn)(void*);
static UpdateFn oUpdate = nullptr;
static MonoMethod* g_addMoneyM = nullptr;
static volatile LONG g_qAmount = 0;
static volatile LONG g_qPending = 0;

static void __cdecl HookUpdate(void* self) {
    // Unity ana thread yedegi: ESP Player.Update hook'u kurulmasa bile spawn kuyrugu calisir.
    fishspawn::Tick();
    slot::Tick();
    if (g_qPending) {
        __try {
            int amt = (int)InterlockedExchange(&g_qAmount, 0);
            InterlockedExchange(&g_qPending, 0);
            if (amt != 0 && g_addMoneyM) {
                MonoClass* pc = MonoAPI::Get().FindClass("", "Player");
                void* lp = pc ? MonoAPI::Get().GetStaticObject(pc, "LocalPlayer") : nullptr;
                if (lp && MonoAPI::Get().IsInstanceOf(lp, pc)) {
                    void* args[2] = { &amt, &lp };
                    MonoAPI::Get().Invoke(g_addMoneyM, nullptr, args);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    silentaim::TickPreview();
    if (oUpdate) oUpdate(self);
}

// ---------- Instance yardimcilari (log'daki Player yapisina gore) ----------
// Player.LocalPlayer (static obje) -> _playerVitals / _inventory / _movement
// DOGRULAMALI: sinif tutmayan pointer'a yazma (crash sebebi #1).
static bool ValidObj(void* obj, const char* klassName) {
    if (!obj) return false;
    auto& mono = MonoAPI::Get();
    MonoClass* c = mono.FindClass("", klassName);
    if (!c) return false;
    return mono.IsInstanceOf(obj, c);
}

static void* GetLocalPlayer() {
    auto& mono = MonoAPI::Get();
    MonoClass* pc = mono.FindClass("", "Player");
    if (!pc) return nullptr;
    const char* names[] = { "LocalPlayer", "<LocalPlayer>k__BackingField",
        "_localPlayer", "localPlayer", "Instance", "_instance", "Local" };
    for (auto* n : names) {
        void* o = mono.GetStaticObject(pc, n);
        if (o && mono.IsInstanceOf(o, pc)) return o; // sinif dogrulamali
    }
    return nullptr;
}

// Numaralandirma yedegi (ayri fonksiyonda: __try ile lambda birlesince C2712 verir)
static bool ScanFieldOffset(MonoClass* klass, const char* field, uint32_t& out) {
    auto& mono = MonoAPI::Get();
    bool found = false;
    mono.ForEachField(klass, [&](MonoClassField* f) {
        if (found) return;
        const char* n = mono.FieldName(f);
        if (n && strcmp(n, field) == 0 && mono.FieldOffsetOf(f, out)) found = true;
    });
    return found;
}

static void* ReadObjField(void* obj, MonoClass* klass, const char* field) {
    if (!obj || !klass) return nullptr;
    auto& mono = MonoAPI::Get();
    uint32_t off = 0;
    if (!mono.GetFieldOffset(klass, field, off) && !ScanFieldOffset(klass, field, off)) return nullptr;
    __try {
        void* p = *(void**)((uint8_t*)obj + off);
        return p;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static bool WriteFloatField(void* obj, MonoClass* klass, const char* field, float v) {
    if (!obj || !klass) return false;
    auto& mono = MonoAPI::Get();
    uint32_t off = 0;
    if (!mono.GetFieldOffset(klass, field, off)) return false;
    __try {
        *(float*)((uint8_t*)obj + off) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool WriteIntField(void* obj, MonoClass* klass, const char* field, int v) {
    if (!obj || !klass) return false;
    auto& mono = MonoAPI::Get();
    uint32_t off = 0;
    if (!mono.GetFieldOffset(klass, field, off)) return false;
    __try {
        *(int*)((uint8_t*)obj + off) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool AddIntField(void* obj, MonoClass* klass, const char* field, int add) {
    if (!obj || !klass) return false;
    auto& mono = MonoAPI::Get();
    uint32_t off = 0;
    if (!mono.GetFieldOffset(klass, field, off)) return false;
    __try {
        int* p = (int*)((uint8_t*)obj + off);
        *p = *p + add;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---------- Ham 4-byte okuma/yazma (POD only -> __try serbest, C2712 yok) ----------
static bool RawRead32(void* obj, uint32_t off, uint32_t& out) {
    __try { out = *(uint32_t*)((uint8_t*)obj + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool RawWrite32(void* obj, uint32_t off, uint32_t v) {
    __try { *(uint32_t*)((uint8_t*)obj + off) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool RawWrite8(void* obj, uint32_t off, uint8_t v) {
    __try { *(uint8_t*)((uint8_t*)obj + off) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool RawRead8(void* obj, uint32_t off, uint8_t& v) {
    __try { v = *(uint8_t*)((uint8_t*)obj + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool RawReadPtr(void* obj, uint32_t off, void*& out) {
    __try { out = *(void**)((uint8_t*)obj + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool RawRead64(void* obj, uint32_t off, uint64_t& out) {
    __try { out = *(uint64_t*)((uint8_t*)obj + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool RawWrite64(void* obj, uint32_t off, uint64_t v) {
    __try { *(uint64_t*)((uint8_t*)obj + off) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static float RawToFloat(uint32_t r) { float f; memcpy(&f, &r, 4); return f; }
static uint32_t FloatToRaw(float f) { uint32_t r; memcpy(&r, &f, 4); return r; }

// Tipi kat'i dogrulanmis bool oku.
static bool ReadBoolField(void* obj, MonoClass* klass, const char* field, bool& out) {
    auto& mono = MonoAPI::Get();
    if (!obj || !klass) return false;
    if (mono.FieldTypeCode(klass, field) != 2) return false;
    uint32_t off = 0;
    if (!mono.GetFieldOffset(klass, field, off)) return false;
    uint8_t v = 0;
    __try { v = *(uint8_t*)((uint8_t*)obj + off); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    out = (v != 0);
    return true;
}

// Tipi kat'i dogrulanmis int yaz (duz int field).
static bool WriteIntChecked(void* obj, MonoClass* klass, const char* field, int v) {
    auto& mono = MonoAPI::Get();
    if (!obj || !klass) return false;
    if (mono.FieldTypeCode(klass, field) != 8) return false; // int degilse dokunma
    uint32_t off = 0;
    if (!mono.GetFieldOffset(klass, field, off)) return false;
    return RawWrite32(obj, off, (uint32_t)v);
}

// SyncVar<int> kutusu icindeki _value int'ini oku/yaz (Cecil ile kanitlandi:
// MoneyManager._money ve vitals _synced* alanlari SyncVar<int> class'i).
static bool SyncVarReadInt(void* syncObj, int& out) {
    auto& mono = MonoAPI::Get();
    MonoClass* sc = mono.ObjectClass(syncObj);
    if (!sc) return false;
    if (mono.FieldTypeCode(sc, "_value") != 8) return false;
    uint32_t off = 0;
    if (!mono.GetFieldOffset(sc, "_value", off)) return false;
    uint32_t v = 0;
    if (!RawRead32(syncObj, off, v)) return false;
    if ((int32_t)v < 0 || v > 50000000u) return false;
    out = (int)v; return true;
}
static bool SyncVarWriteInt(void* syncObj, int v) {
    auto& mono = MonoAPI::Get();
    MonoClass* sc = mono.ObjectClass(syncObj);
    if (!sc) return false;
    if (mono.FieldTypeCode(sc, "_value") != 8) return false;
    uint32_t off = 0;
    if (!mono.GetFieldOffset(sc, "_value", off)) return false;
    if (!RawWrite32(syncObj, off, (uint32_t)v)) return false;
    int back = 0;
    return SyncVarReadInt(syncObj, back) && back == v;
}
// Objenin SyncVar<int> field'ina int yaz (ornegin vitals._syncedHealth)
static bool SyncVarWriteOnField(void* obj, MonoClass* klass, const char* field, int v) {
    void* sync = ReadObjField(obj, klass, field);
    if (!sync) return false;
    return SyncVarWriteInt(sync, v);
}

// TIP KAPISI: sadece 4-byte numeriklere (int/uint/float) 4-byte yazilir.
// Obje referansi / bool / double'a yazmak oyunu devirir (F6 crash sebebi buydu).
static bool IsInt32(MonoClass* c, const char* f) {
    int t = MonoAPI::Get().FieldTypeCode(c, f);
    return t == 8 || t == 9;
}
static bool IsNum32(MonoClass* c, const char* f) {
    int t = MonoAPI::Get().FieldTypeCode(c, f);
    return t == 8 || t == 9 || t == 12;
}

// Hasar carpani uygula: float ise float carpar, int ise int carpar, oneShot ise 99999.
// baseRaw daha once okunmus orijinal degerdir.
static void ApplyDamageMult(void* obj, uint32_t off, int dmgIndex, uint32_t baseRaw) {
    float mult = 1.f; bool oneShot = false;
    switch (dmgIndex) {
        case 1: mult = 2.f; break;
        case 2: mult = 5.f; break;
        case 3: mult = 10.f; break;
        default: oneShot = true; break;
    }
    float bf = RawToFloat(baseRaw);
    bool saneFloat = (bf > 0.001f && bf < 100000.f);
    if (saneFloat && !oneShot) {
        float nv = bf * mult;
        RawWrite32(obj, off, FloatToRaw(nv));
    } else if (oneShot) {
        if (saneFloat) { float f = 99999.f; RawWrite32(obj, off, FloatToRaw(f)); }
        else RawWrite32(obj, off, 99999);
    } else {
        if (baseRaw >= 1 && baseRaw <= 10000) RawWrite32(obj, off, baseRaw * (uint32_t)mult);
    }
}

// _money cozumu v16: isim aramasi guvenilmez oldugu icin, numaralandirmada
// GORUNEN field pointer'i onbelge alinir ve offset/tip dogrudan ondan okunur.
static MonoClassField* g_moneyF = nullptr;
static bool MoneyFieldResolve(std::string* why = nullptr) {
    auto& mono = MonoAPI::Get();
    if (g_moneyF) return true;
    MonoClass* mm = mono.FindClass("", "MoneyManager");
    if (!mm) { if (why) *why = "sinif yok"; return false; }
    mono.ForEachField(mm, [&](MonoClassField* f) {
        if (g_moneyF) return;
        const char* n = mono.FieldName(f);
        if (n && strcmp(n, "_money") == 0) g_moneyF = f;
    });
    if (!g_moneyF && why) *why = "enum'de yok";
    return g_moneyF != nullptr;
}
static bool ReadMoneyValue(void* inst, MonoClass* mm, uint64_t& out) {
    auto& mono = MonoAPI::Get();
    uint32_t off = 0;
    // 0) KANITLANMIS YOL: _money SyncVar<int> kutusu -> _value oku
    {
        void* sync = ReadObjField(inst, mm, "_money");
        int sv = 0;
        if (sync && SyncVarReadInt(sync, sv)) { out = (uint64_t)sv; return true; }
    }
    // 1) onbellekli enumerated pointer (birincil yol)
    if (MoneyFieldResolve()) {
        int t = mono.FieldTypeOf(g_moneyF);
        if ((t == 8 || t == 9) && mono.FieldOffsetOf(g_moneyF, off)) {
            uint32_t v = 0;
            if (RawRead32(inst, off, v) && (int32_t)v >= 0 && v <= 50000000u) { out = v; return true; }
            return false;
        }
        if (t == 12 && mono.FieldOffsetOf(g_moneyF, off)) {
            uint32_t r = 0;
            if (!RawRead32(inst, off, r)) return false;
            float f = RawToFloat(r);
            if (f < 0.f || f > 50000000.f) return false;
            out = (uint64_t)f; return true;
        }
        if ((t == 10 || t == 11) && mono.FieldOffsetOf(g_moneyF, off)) {
            uint64_t v = 0;
            if (!RawRead64(inst, off, v)) return false;
            if ((int64_t)v < 0 || v > 50000000ull) return false;
            out = v; return true;
        }
        // Tip bilinmiyor (-1/generic): probe oku, mantikliysa dondur (sadece okuma, risksiz)
        if (mono.FieldOffsetOf(g_moneyF, off)) {
            uint32_t v = 0;
            if (RawRead32(inst, off, v) && (int32_t)v >= 0 && v <= 50000000u) { out = v; return true; }
        }
        return false;
    }
    // 2) yedek: isim yolu
    int t = mono.FieldTypeCode(mm, "_money");
    if ((t == 8 || t == 9) && mono.GetFieldOffset(mm, "_money", off)) {
        uint32_t v = 0;
        if (RawRead32(inst, off, v) && (int32_t)v >= 0 && v <= 50000000u) { out = v; return true; }
        return false;
    }
    if ((t == 12) && mono.GetFieldOffset(mm, "_money", off)) {
        uint32_t r = 0;
        if (!RawRead32(inst, off, r)) return false;
        float f = RawToFloat(r);
        if (f < 0.f || f > 50000000.f) return false;
        out = (uint64_t)f; return true;
    }
    if ((t == 10 || t == 11) && mono.GetFieldOffset(mm, "_money", off)) {
        uint64_t v = 0;
        if (!RawRead64(inst, off, v)) return false;
        if ((int64_t)v < 0 || v > 50000000ull) return false;
        out = v; return true;
    }
    return false;
}

static bool WriteMoneyValue(void* inst, MonoClass* mm, uint64_t v) {
    auto& mono = MonoAPI::Get();
    uint32_t off = 0;
    // 0) KANITLANMIS YOL: SyncVar<int> kutusuna yaz
    {
        void* sync = ReadObjField(inst, mm, "_money");
        if (sync && SyncVarWriteInt(sync, (int)v)) return true;
    }
    if (MoneyFieldResolve()) {
        int t = mono.FieldTypeOf(g_moneyF);
        if ((t == 8 || t == 9) && mono.FieldOffsetOf(g_moneyF, off))
            return RawWrite32(inst, off, (uint32_t)v);
        if (t == 12 && mono.FieldOffsetOf(g_moneyF, off))
            return RawWrite32(inst, off, FloatToRaw((float)v));
        if ((t == 10 || t == 11) && mono.FieldOffsetOf(g_moneyF, off))
            return RawWrite64(inst, off, v);
        // Tip bilinmiyor: probe + backing yakinligi cift kapisi.
        // Pointer'lar 50M filtresine takilir; ayni miktarin iki gorunumu 1M'dan yakin olur.
        if (mono.FieldOffsetOf(g_moneyF, off)) {
            uint32_t probe = 0, braw = 0;
            uint32_t boff = 0;
            bool okP = RawRead32(inst, off, probe) && (int32_t)probe >= 0 && probe <= 50000000u;
            bool okB = IsInt32(mm, "<Money>k__BackingField") &&
                       mono.GetFieldOffset(mm, "<Money>k__BackingField", boff) &&
                       RawRead32(inst, boff, braw) && (int32_t)braw >= 0 && braw <= 50000000u;
            if (okP && okB) {
                uint32_t diff = probe > braw ? probe - braw : braw - probe;
                if (diff <= 1000000u) return RawWrite32(inst, off, (uint32_t)v);
            }
        }
        return false;
    }
    int t = mono.FieldTypeCode(mm, "_money");
    if ((t == 8 || t == 9) && mono.GetFieldOffset(mm, "_money", off))
        return RawWrite32(inst, off, (uint32_t)v);
    if (t == 12 && mono.GetFieldOffset(mm, "_money", off))
        return RawWrite32(inst, off, FloatToRaw((float)v));
    if ((t == 10 || t == 11) && mono.GetFieldOffset(mm, "_money", off))
        return RawWrite64(inst, off, v);
    return false;
}

// _money struct ici slot: backing degerine esit olan slot (otomatik, -1=bilinmiyor)
static int g_moneySlot = -1;
// Bir onceki tarama anlik goruntu (degisimden slot bulmak icin)
static uint32_t g_slotSnap[8] = {0};
static bool g_haveSnap = false;

// _money struct ise icini tara: HUD'daki deger hangi slotta?
// (Isim aramasi degil, enum pointer offset'i kullanilir.)
static void ScanMoneyStruct(void* inst, MonoClass* mm) {
    (void)mm;
    auto& s = State();
    auto& mono = MonoAPI::Get();
    uint32_t off = 0;
    if (!MoneyFieldResolve() || !mono.FieldOffsetOf(g_moneyF, off)) {
        s.Log("[!] _money offset yok, scan yapilamadi.");
        return;
    }
    std::string line = ">> _money struct icerik [slot:int/float]: ";
    for (int i = 0; i < 8; ++i) {
        uint32_t r = 0;
        if (!RawRead32(inst, off + (uint32_t)(i * 4), r)) break;
        float f = RawToFloat(r);
        char tmp[64];
        sprintf_s(tmp, "[%d]%d/%.2f ", i, (int32_t)r, (double)f);
        line += tmp;
        if (line.size() > 400) break;
    }
    s.Log(line);
}

// Ref para hedefi (Tick kilidi icin saklanir)
static void* g_mrefObj = nullptr;
static MonoClass* g_mrefCls = nullptr;
static uint32_t g_mrefOff = 0;

// _money obje referansiysa icindeki int'i bul (double-deref: P=*(inst+232), tara P+16..).
// Benzersiz backing eslesmesi olursa yazar. Donus basariysa outNew=yeni deger,
// hedef g_mrefObj/g_mrefOff'a kaydedilir (Tick kilidi kullanir).
static bool TryMoneyRefPath(void* inst, uint32_t base, uint32_t backing, uint32_t amount, uint64_t& outNew) {
    auto& s = State();
    auto& mono = MonoAPI::Get();
    void* P = nullptr;
    if (!RawReadPtr(inst, base, P) || !P) return false;
    void* kp = nullptr;
    if (!RawReadPtr(P, 0, kp) || !kp) return false;
    char pb[128];
    sprintf_s(pb, ">> _money ref: P=0x%p klass=0x%p", P, kp);
    s.Log(pb);
    std::string line = ">> ref icerik [+16..]: ";
    int found = -1, cnt = 0;
    for (int i = 0; i < 12; ++i) {
        uint32_t r = 0;
        if (!RawRead32(P, 16u + (uint32_t)(i * 4), r)) break;
        if (i < 6) { char tmp[48]; sprintf_s(tmp, "[+%d]%d ", 16 + i * 4, (int32_t)r); line += tmp; }
        if ((int32_t)r < 0 || r > 50000000u) continue;
        if (r == backing) { found = i; ++cnt; }
    }
    s.Log(line);
    if (cnt != 1) return false;
    uint32_t addr = 16u + (uint32_t)(found * 4);
    uint32_t cur = 0;
    if (!RawRead32(P, addr, cur)) return false;
    uint32_t nv = cur + amount;
    if (!RawWrite32(P, addr, nv)) return false;
    uint32_t back = 0;
    if (!RawRead32(P, addr, back) || back != nv) return false;
    MonoClass* pc = mono.ObjectClass(P);
    char sb[128];
    sprintf_s(sb, ">> ref slot +%u secildi", addr);
    s.Log(sb);
    g_mrefObj = P; g_mrefCls = pc; g_mrefOff = addr;
    outNew = nv;
    return true;
}

// HUD sayisal alanlarina yaz, dokunulan sayisini dondur
static int TouchMoneyUI(uint64_t nv) {
    auto& mono = MonoAPI::Get();
    int touched = 0;
    MonoClass* uic = mono.FindClass("", "PlayerUI");
    if (!uic) return 0;
    void* ui = mono.GetStaticObject(uic, "_instance");
    if (ui && !mono.IsInstanceOf(ui, uic)) ui = nullptr; // bayat UI
    void* mui = ui ? ReadObjField(ui, uic, "_moneyUI") : nullptr;
    MonoClass* muic = mui ? mono.ObjectClass(mui) : nullptr;
    if (muic && !mono.IsInstanceOf(mui, muic)) muic = nullptr;
    if (!muic) return 0;
    const char* muf[] = { "_curMoney", "_targetMoney", "_prevMoney" };
    for (auto* f : muf) {
        if (!IsInt32(muic, f)) continue; // float/obje ise dokunma (crash yapar)
        uint32_t fo = 0, rv = 0;
        if (!mono.GetFieldOffset(muic, f, fo)) continue;
        if (!RawRead32(mui, fo, rv)) continue;
        if ((int32_t)rv < 0 || rv > 50000000u) continue;
        RawWrite32(mui, fo, (uint32_t)nv);
        ++touched;
    }
    return touched;
}

static void WriteBacking(void* inst, MonoClass* mm, uint64_t nv) {
    auto& mono = MonoAPI::Get();
    uint32_t off = 0;
    if (IsInt32(mm, "<Money>k__BackingField") && mono.GetFieldOffset(mm, "<Money>k__BackingField", off))
        RawWrite32(inst, off, (uint32_t)nv);
}

// backing degerine BENZERSIZ eslesen slotu bul (sifirlar karisikken -1 doner)
static int FindMoneySlot(void* inst, uint32_t base, uint32_t backing) {
    int found = -1, count = 0;
    for (int i = 0; i < 8; ++i) {
        uint32_t r = 0;
        if (!RawRead32(inst, base + (uint32_t)(i * 4), r)) break;
        if ((int32_t)r < 0 || r > 50000000u) continue;
        if (r == backing) { found = i; ++count; }
    }
    return count == 1 ? found : -1;
}

static bool ReadMoneySlot(void* inst, uint32_t base, int slot, uint64_t& out) {
    uint32_t v = 0;
    if (!RawRead32(inst, base + (uint32_t)(slot * 4), v)) return false;
    if ((int32_t)v < 0 || v > 50000000u) return false;
    out = v; return true;
}

static bool WriteMoneySlot(void* inst, uint32_t base, int slot, uint32_t v) {
    return RawWrite32(inst, base + (uint32_t)(slot * 4), v);
}
static bool CopyField32(void* obj, MonoClass* klass, const char* from, const char* to) {
    if (!obj || !klass) return false;
    if (!IsNum32(klass, from)) return false;
    auto& mono = MonoAPI::Get();
    if (mono.FieldTypeCode(klass, from) != mono.FieldTypeCode(klass, to)) return false;
    uint32_t of = 0, ot = 0;
    if (!mono.GetFieldOffset(klass, from, of)) return false;
    if (!mono.GetFieldOffset(klass, to, ot)) return false;
    uint32_t v = 0;
    if (!RawRead32(obj, of, v)) return false;
    return RawWrite32(obj, ot, v);
}
static bool ZeroField(void* obj, MonoClass* klass, const char* name) {
    if (!obj || !klass) return false;
    if (!IsNum32(klass, name)) return false;
    auto& mono = MonoAPI::Get();
    uint32_t off = 0;
    if (!mono.GetFieldOffset(klass, name, off)) return false;
    return RawWrite32(obj, off, 0);
}

// PlayerHolding wrapper'sa bir katman asagi in (_heldItem): gercek silahi bul.
// Sadece silaha benziyorsa (Ammo/_weaponInfo alani varsa) inilir, yoksa oldugu gibi kalir.
static void ResolveWeapon(void* hold, MonoClass* hc, void** outObj, MonoClass** outCls) {
    auto& mono = MonoAPI::Get();
    *outObj = hold; *outCls = hc;
    if (!hold || !hc) return;
    uint32_t off = 0;
    if (!mono.GetFieldOffset(hc, "_heldItem", off)) return;
    void* child = nullptr;
    if (!RawReadPtr(hold, off, child) || !child) return;
    MonoClass* cc = mono.ObjectClass(child);
    if (!cc) return;
    uint32_t t = 0;
    if (mono.GetFieldOffset(cc, "<Ammo>k__BackingField", t) || mono.GetFieldOffset(cc, "_weaponInfo", t)) {
        *outObj = child; *outCls = cc;
    }
}

// No-recoil instance alanlari: patch sadece recoil metodlarini susturur;
// projectile spread ve fiziksel knockback Weapon alanlarindan okunur.
static void* g_nrWeapon = nullptr;
static MonoClass* g_nrWeaponClass = nullptr;
static uint32_t g_nrSpreadOff = 0, g_nrKnockOff = 0;
static uint32_t g_nrSpreadRaw = 0, g_nrKnockRaw = 0;
static bool g_nrSpreadSaved = false, g_nrKnockSaved = false;
static void* g_nrWeaponInfo = nullptr;
static MonoClass* g_nrWeaponInfoClass = nullptr;
static uint32_t g_nrGravityOff = 0, g_nrGravityRaw = 0;
static bool g_nrGravitySaved = false;

struct BarrelRecoilBackup {
    void* obj = nullptr;
    MonoClass* klass = nullptr;
    uint32_t screenOff = 0, posOff = 0, rotOff = 0, multiOff = 0;
    uint32_t screen[2]{}, pos[3]{}, rot[2]{}, multi = 0;
    bool screenSaved = false, posSaved = false, rotSaved = false, multiSaved = false;
};
static std::vector<BarrelRecoilBackup> g_nrBarrels;

static int ManagedListCount(void* list) {
    auto& mono = MonoAPI::Get();
    MonoClass* lc = mono.ObjectClass(list);
    MonoMethod* m = lc ? mono.FindMethod(lc, "get_Count", 0) : nullptr;
    return m ? mono.UnboxInt(mono.Invoke(m, list, nullptr)) : 0;
}

static void* ManagedListItem(void* list, int index) {
    auto& mono = MonoAPI::Get();
    MonoClass* lc = mono.ObjectClass(list);
    MonoMethod* m = lc ? mono.FindMethod(lc, "get_Item", 1) : nullptr;
    void* args[1] = { &index };
    return m ? mono.Invoke(m, list, args) : nullptr;
}

static void RestoreNoRecoilBarrels() {
    auto& mono = MonoAPI::Get();
    for (auto& b : g_nrBarrels) {
        if (!b.obj || !b.klass || !mono.IsInstanceOf(b.obj, b.klass)) continue;
        if (b.screenSaved) { RawWrite32(b.obj, b.screenOff, b.screen[0]); RawWrite32(b.obj, b.screenOff + 4, b.screen[1]); }
        if (b.posSaved) { RawWrite32(b.obj, b.posOff, b.pos[0]); RawWrite32(b.obj, b.posOff + 4, b.pos[1]); RawWrite32(b.obj, b.posOff + 8, b.pos[2]); }
        if (b.rotSaved) { RawWrite32(b.obj, b.rotOff, b.rot[0]); RawWrite32(b.obj, b.rotOff + 4, b.rot[1]); }
        if (b.multiSaved) RawWrite32(b.obj, b.multiOff, b.multi);
    }
    g_nrBarrels.clear();
}

static void ZeroNoRecoilBarrels(void* weapon, MonoClass* wc) {
    auto& mono = MonoAPI::Get();
    void* attachments = ReadObjField(weapon, wc, "_attachments");
    MonoClass* ac = attachments ? mono.ObjectClass(attachments) : nullptr;
    void* list = (attachments && ac) ? ReadObjField(attachments, ac, "_barrelAttachments") : nullptr;
    int count = ManagedListCount(list);
    if (count <= 0 || count > 64) return;
    for (int i = 0; i < count; ++i) {
        void* barrel = ManagedListItem(list, i);
        MonoClass* bc = barrel ? mono.ObjectClass(barrel) : nullptr;
        if (!barrel || !bc) continue;
        BarrelRecoilBackup* found = nullptr;
        for (auto& b : g_nrBarrels) if (b.obj == barrel) { found = &b; break; }
        if (!found) {
            BarrelRecoilBackup b; b.obj = barrel; b.klass = bc;
            if (mono.GetFieldOffset(bc, "_screenRecoilAmount", b.screenOff))
                b.screenSaved = RawRead32(barrel, b.screenOff, b.screen[0]) && RawRead32(barrel, b.screenOff + 4, b.screen[1]);
            if (mono.GetFieldOffset(bc, "_modelRecoilPos", b.posOff))
                b.posSaved = RawRead32(barrel, b.posOff, b.pos[0]) && RawRead32(barrel, b.posOff + 4, b.pos[1]) && RawRead32(barrel, b.posOff + 8, b.pos[2]);
            if (mono.GetFieldOffset(bc, "_modelRecoilRot", b.rotOff))
                b.rotSaved = RawRead32(barrel, b.rotOff, b.rot[0]) && RawRead32(barrel, b.rotOff + 4, b.rot[1]);
            if (mono.GetFieldOffset(bc, "_weaponRecoilMulti", b.multiOff))
                b.multiSaved = RawRead32(barrel, b.multiOff, b.multi);
            g_nrBarrels.push_back(b);
            found = &g_nrBarrels.back();
            static bool logged = false;
            if (!logged && (b.screenSaved || b.posSaved || b.rotSaved || b.multiSaved)) {
                State().Log("[+] BarrelAttachment recoil kaynaklari bulundu ve sifirlandi.");
                logged = true;
            }
        }
        if (found->screenSaved) { RawWrite32(barrel, found->screenOff, 0); RawWrite32(barrel, found->screenOff + 4, 0); }
        if (found->posSaved) { RawWrite32(barrel, found->posOff, 0); RawWrite32(barrel, found->posOff + 4, 0); RawWrite32(barrel, found->posOff + 8, 0); }
        if (found->rotSaved) { RawWrite32(barrel, found->rotOff, 0); RawWrite32(barrel, found->rotOff + 4, 0); }
        if (found->multiSaved) RawWrite32(barrel, found->multiOff, 0);
    }
}

static void RestoreNoRecoilWeapon() {
    auto& mono = MonoAPI::Get();
    if (g_nrWeapon && g_nrWeaponClass && mono.IsInstanceOf(g_nrWeapon, g_nrWeaponClass)) {
        if (g_nrSpreadSaved) RawWrite32(g_nrWeapon, g_nrSpreadOff, g_nrSpreadRaw);
        if (g_nrKnockSaved) RawWrite32(g_nrWeapon, g_nrKnockOff, g_nrKnockRaw);
    }
    if (g_nrWeaponInfo && g_nrWeaponInfoClass && g_nrGravitySaved &&
        mono.IsInstanceOf(g_nrWeaponInfo, g_nrWeaponInfoClass))
        RawWrite32(g_nrWeaponInfo, g_nrGravityOff, g_nrGravityRaw);
    g_nrWeapon = nullptr; g_nrWeaponClass = nullptr;
    g_nrSpreadSaved = g_nrKnockSaved = false;
    g_nrWeaponInfo = nullptr; g_nrWeaponInfoClass = nullptr; g_nrGravitySaved = false;
}

static void ApplyNoRecoilWeapon(void* weapon, MonoClass* wc) {
    auto& mono = MonoAPI::Get();
    if (!weapon || !wc) return;
    if (weapon != g_nrWeapon) {
        RestoreNoRecoilWeapon();
        g_nrWeapon = weapon; g_nrWeaponClass = wc;
        if (mono.GetFieldOffset(wc, "_spread", g_nrSpreadOff))
            g_nrSpreadSaved = RawRead32(weapon, g_nrSpreadOff, g_nrSpreadRaw);
        if (mono.GetFieldOffset(wc, "_recoilKnockback", g_nrKnockOff))
            g_nrKnockSaved = RawRead32(weapon, g_nrKnockOff, g_nrKnockRaw);
        g_nrWeaponInfo = ReadObjField(weapon, wc, "_weaponInfo");
        g_nrWeaponInfoClass = g_nrWeaponInfo ? mono.ObjectClass(g_nrWeaponInfo) : nullptr;
        if (g_nrWeaponInfo && g_nrWeaponInfoClass &&
            mono.GetFieldOffset(g_nrWeaponInfoClass, "<ProjectileGravity>k__BackingField", g_nrGravityOff))
            g_nrGravitySaved = RawRead32(g_nrWeaponInfo, g_nrGravityOff, g_nrGravityRaw);
        if (g_nrSpreadSaved || g_nrKnockSaved || g_nrGravitySaved)
            State().Log("[+] Weapon spread/knockback/projectile-gravity sifirlandi.");
    }
    if (g_nrSpreadSaved) RawWrite32(weapon, g_nrSpreadOff, 0);
    if (g_nrKnockSaved) RawWrite32(weapon, g_nrKnockOff, 0);
    if (g_nrGravitySaved && g_nrWeaponInfo) RawWrite32(g_nrWeaponInfo, g_nrGravityOff, 0);
}

static void ClearCameraRecoil(void* player, MonoClass* pc) {
    auto& mono = MonoAPI::Get();
    void* camera = ReadObjField(player, pc, "_camera");
    MonoClass* cc = camera ? mono.ObjectClass(camera) : nullptr;
    if (!camera || !cc) return;
    uint32_t off = 0;
    if (mono.GetFieldOffset(cc, "_recoilTar", off)) {
        RawWrite32(camera, off, 0); RawWrite32(camera, off + 4, 0);
    }
    if (mono.GetFieldOffset(cc, "_recoilCur", off)) {
        RawWrite32(camera, off, 0); RawWrite32(camera, off + 4, 0);
    }
}

// Update hook kurulumu (mono hazir + MinHook hazir olduktan sonra bir kere).
// Basariliysa F6 miktari resmi AddMoney(int, Player) ile main thread'de isler.
bool InstallUpdateHook() {
    auto& s = State();
    auto& mono = MonoAPI::Get();
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { s.Log("[!] MinHook init olmadi."); return false; }
    MonoClass* mm = mono.FindClass("", "MoneyManager");
    if (!mm) return false;
    MonoMethod* upd = mono.FindMethod(mm, "Update", 0);
    if (!upd) upd = mono.FindMethodAny(mm, "Update");
    if (!upd) { s.Log("[!] Update bulunamadi."); return false; }
    void* code = mono.CompileMethod(upd);
    if (!code) { s.Log("[!] Update JIT alinamadi."); return false; }
    MonoMethod* add = nullptr;
    mono.ForEachMethod(mm, [&](MonoMethod* m) {
        if (add) return;
        const char* n = mono.MethodName(m);
        if (!n || strcmp(n, "AddMoney") != 0) return;
        if (mono.ParamCount(m) != 2) return;
        if (!mono.ParamIsInt(m, 0)) return;
        add = m;
    });
    // AddMoney bulunamasa da Update hook'u canli ESP'nin main-thread tick'i icin
    // kurulmaya devam eder; para ekleme bu durumda mevcut ham yola duser.
    if (!add) s.Log("[!] AddMoney(int,*) bulunamadi; para ham yol, canli ESP aktif.");
    g_addMoneyM = add;
    if (MH_CreateHook(code, &HookUpdate, (void**)&oUpdate) != MH_OK) {
        s.Log("[!] Update hook kurulamadi."); g_addMoneyM = nullptr; return false;
    }
    if (MH_EnableHook(code) != MH_OK) {
        s.Log("[!] Update hook acilamadi."); g_addMoneyM = nullptr; return false;
    }
    s.Log("[+] Resmi para yolu aktif (F6 miktari oyunun kendi fonksiyonuyla eklenecek)");
    return true;
}

// Para v27: SECILEN miktar RESMI AddMoney(int, Player) ile (main-thread kuyrugu).
// Hook yoksa ham SyncVar yoluna duser (calistigi kanitli).
bool AddMoney(int amount) {
    auto& s = State();
    auto& mono = MonoAPI::Get();
    if (!mono.IsReady()) { s.Log("Mono hazir degil."); return false; }
    if (amount <= 0) { s.Log("[!] Miktar 0'dan buyuk olmali."); return false; }
    MonoClass* mm = mono.FindClass("", "MoneyManager");
    if (!mm) { s.Log("[!] MoneyManager yok."); return false; }
    void* inst = mono.GetStaticObject(mm, "Instance");
    if (!inst) { s.Log("[!] Instance null (lobide degilsin)."); return false; }
    if (!mono.IsInstanceOf(inst, mm)) { s.Log("[!] Instance bayat."); return false; }
    // Birincil yol: resmi kuyruk (hook kuruluysa). Miktar oyunun kendi
    // AddMoney(int, Player) fonksiyonuyla main thread'de islenir.
    if (g_addMoneyM) {
        InterlockedExchange(&g_qAmount, (LONG)amount);
        InterlockedExchange(&g_qPending, 1);
        char qb[128];
        sprintf_s(qb, "[+] Resmi kuyrukta: +%d", amount);
        s.Log(qb);
        return true;
    }
    void* sync = ReadObjField(inst, mm, "_money");
    int cur = 0;
    if (!sync || !SyncVarReadInt(sync, cur)) { s.Log("[!] Para okunamadi."); return false; }
    {
        // Server mi client kopyasi mi? (host'ta ikisi de vardir)
        bool srv = false, cli = false;
        ReadBoolField(inst, mm, "_initializedOnceServer", srv);
        ReadBoolField(inst, mm, "_initializedOnceClient", cli);
        char nb[96];
        sprintf_s(nb, ">> net: server=%d client=%d sync=%s", srv ? 1 : 0, cli ? 1 : 0, mono.ObjClassName(sync));
        s.Log(nb);
    }
    int nv = cur + amount;
    if (!SyncVarWriteInt(sync, nv)) { s.Log("[!] Para yazilamadi."); return false; }
    int back = 0;
    bool ok = SyncVarReadInt(sync, back) && back == nv;
    WriteBacking(inst, mm, nv);
    int touched = TouchMoneyUI(nv);
    // NOT: otomatik kilit YOK (gozlem icin). Deger duruyor mu diye Parayi Oku ile bak.
    s.Log(std::string("[+] Para: ") + std::to_string(cur) + " -> " + std::to_string(nv) +
          (ok ? " [tuttu]" : " [UYARI]") + " UI:" + std::to_string(touched));
    return true;
}

// Sadece okur: _money SyncVar<int> kutusundan (resmi depodan).
bool LogMoney() {
    auto& s = State();
    auto& mono = MonoAPI::Get();
    if (!mono.IsReady()) { s.Log("Mono hazir degil."); return false; }
    MonoClass* mm = mono.FindClass("", "MoneyManager");
    if (!mm) { s.Log("[!] MoneyManager yok."); return false; }
    void* inst = mono.GetStaticObject(mm, "Instance");
    if (!inst) { s.Log("[!] Instance null (lobide degilsin)."); return false; }
    if (!mono.IsInstanceOf(inst, mm)) { s.Log("[!] Instance bayat."); return false; }
    void* sync = ReadObjField(inst, mm, "_money");
    int cur = 0;
    if (sync && SyncVarReadInt(sync, cur)) {
        s.Log(std::string("[=] Mevcut para: ") + std::to_string(cur));
        return true;
    }
    s.Log("[!] Para okunamadi."); return false;
}

// Frame hook icinden, Player.Update bittikten sonra cagrilir. Boylece oyun ayni frame'de
// Velocity.y degerini ezmez. Hook bulunamazsa dllmain'deki 50ms yol yedek kalir.
void AirJumpTick() {
    auto& s = State();
    if (!s.airJump) return;
    // Infinite Jump: basili tutup ucma degil, her yeni SPACE basiminda bir kez zipla.
    static bool wasSpace = false;
    bool space = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    bool pressed = space && !wasSpace;
    wasSpace = space;
    if (!pressed) return;
    auto& mono = MonoAPI::Get();
    void* lp = GetLocalPlayer();
    if (!lp) return;
    MonoClass* pc = mono.FindClass("", "Player");
    if (!pc) return;
    void* mov = ReadObjField(lp, pc, "_movement");
    if (!mov) return;
    MonoClass* mc = mono.ObjectClass(mov);
    if (!mc) return;

    // Oyunun kendi Jump() kontrolu grounded/hasJumped kapilarini kullaniyor.
    // Bunlari sadece cagrinin suresince gecici ac; sonra orijinal grounded durumunu geri koy.
    struct BoolBackup { uint32_t off; uint8_t value; bool valid; } grounded[2]{};
    const char* groundedNames[] = { "_isGrounded", "<Grounded>k__BackingField" };
    for (int i = 0; i < 2; ++i) {
        uint32_t off = 0; uint8_t old = 0;
        if (mono.FieldTypeCode(mc, groundedNames[i]) == 2 &&
            mono.GetFieldOffset(mc, groundedNames[i], off) && RawRead8(mov, off, old)) {
            grounded[i] = { off, old, true };
            RawWrite8(mov, off, 1);
        }
    }
    const char* blockers[] = { "_hasJumped", "_isJumping" };
    for (auto* name : blockers) {
        uint32_t off = 0;
        if (mono.FieldTypeCode(mc, name) == 2 && mono.GetFieldOffset(mc, name, off))
            RawWrite8(mov, off, 0);
    }
    uint32_t jumpsOff = 0;
    if (mono.FieldTypeCode(mc, "_jumpsLeft") == 8 && mono.GetFieldOffset(mc, "_jumpsLeft", jumpsOff))
        RawWrite32(mov, jumpsOff, 1);

    MonoMethod* jump = mono.FindMethod(mc, "Jump", 0);
    if (jump) mono.Invoke(jump, mov, nullptr);

    // Sahte grounded sadece Jump() kontrolu icindi; havadaki fizik durumunu bozma.
    for (auto& b : grounded) if (b.valid) RawWrite8(mov, b.off, b.value);

    static MonoClass* cachedClass = nullptr;
    static uint32_t cachedOff = 0;
    static bool cachedValid = false;
    if (mc != cachedClass) { cachedClass = mc; cachedOff = 0; cachedValid = false; }
    if (!cachedValid) {
        const char* velocityFields[] = { "<Velocity>k__BackingField", "_velocity", "velocity", "Velocity" };
        for (auto* name : velocityFields) {
            uint32_t candidate = 0;
            if (mono.GetFieldOffset(mc, name, candidate) && mono.FieldTypeCode(mc, name) == 17) {
                cachedOff = candidate;
                cachedValid = true;
                break;
            }
        }
    }
    if (!cachedValid) return;
    RawWrite32(mov, cachedOff + 4, FloatToRaw(8.0f)); // Vector3.y = 8
}

// Sonsuz mermi/no-reload native frame yolu. 50ms Tick'te ammo yazmak arasinda kalan
// bir frame'de reload baslatabiliyordu; simdi Player.Update sonrasinda aninda bastirilir.
void NoReloadFrameTick() {
    auto& s = State();
    if (!s.infAmmo) return;
    auto& mono = MonoAPI::Get();
    void* lp = GetLocalPlayer();
    MonoClass* pc = mono.FindClass("", "Player");
    if (!lp || !pc) return;
    void* hold = ReadObjField(lp, pc, "_holding");
    if (!hold) return;
    MonoClass* hc = mono.ObjectClass(hold);
    if (!hc) return;
    void* weapon = hold; MonoClass* wc = hc;
    ResolveWeapon(hold, hc, &weapon, &wc);
    uint32_t off = 0;
    if (IsInt32(wc, "<Ammo>k__BackingField") && mono.GetFieldOffset(wc, "<Ammo>k__BackingField", off))
        RawWrite32(weapon, off, 999);
    const char* reloadFlags[] = { "_isReloading", "<IsReloading>k__BackingField", "isReloading" };
    for (auto* name : reloadFlags) {
        if (mono.FieldTypeCode(wc, name) == 2 && mono.GetFieldOffset(wc, name, off))
            RawWrite8(weapon, off, 0);
    }
}

// Her frame: JIT tutmasa bile instance uzerinden can/aclik/mermi/hiz/hasar tazele.
// Tip varsayimindan kacinmak icin ham kopya + sifir kullanilir (int/float ikisinde de guvenli).
void Tick() {
    auto& s = State();
    auto& mono = MonoAPI::Get();
    if (!mono.IsReady()) return;

    // --- Para slotu OTOMATIK kesif (salt-okunur, ~500ms'de bir) ---
    // Kullanicinin bir sey yapmasi gerekmez: normal oyunda para degistigi an
    // degisen tek slot bulunur ve F6 o slotu kullanir.
    {
        static int tickDiv = 0;
        if (g_moneySlot < 0 && (++tickDiv % 10 == 0)) {
            MonoClass* mmw = mono.FindClass("", "MoneyManager");
            if (mmw) {
                void* winst = mono.GetStaticObject(mmw, "Instance");
                if (winst && mono.IsInstanceOf(winst, mmw) && MoneyFieldResolve()) {
                    uint32_t wbase = 0;
                    if (mono.FieldOffsetOf(g_moneyF, wbase)) {
                        uint32_t cur8[8];
                        bool okR = true;
                        for (int i = 0; i < 8; ++i)
                            if (!RawRead32(winst, wbase + (uint32_t)(i * 4), cur8[i])) { okR = false; break; }
                        if (okR) {
                            if (g_haveSnap) {
                                int found = -1, cnt = 0;
                                for (int i = 0; i < 8; ++i) {
                                    if (cur8[i] == g_slotSnap[i]) continue;
                                    if ((int32_t)cur8[i] < 0 || cur8[i] > 50000000u) continue;
                                    if ((int32_t)g_slotSnap[i] < 0 || g_slotSnap[i] > 50000000u) continue;
                                    found = i; ++cnt;
                                }
                                if (cnt == 1) {
                                    g_moneySlot = found;
                                    char wb[128];
                                    sprintf_s(wb, "[+] Para slotu otomatik bulundu: %d (F6 calisir)", found);
                                    s.Log(wb);
                                }
                            }
                            memcpy(g_slotSnap, cur8, sizeof(g_slotSnap));
                            g_haveSnap = true;
                        }
                    }
                }
            }
        }
    }

    if (!s.godMode && !s.noHunger && !s.noRecoil && !g_nrWeapon && g_nrBarrels.empty() && !s.infAmmo && !s.speedHack && !s.moneyLock &&
        s.dmgIndex == 0 && !s.dmgCached && !s.projCached) return;

    void* lp = GetLocalPlayer();
    if (!lp) return;
    MonoClass* pc = mono.FindClass("", "Player");
    if (!pc) return;

    // --- Para kilidi: SyncVar<int> kutusu + backing hedefte tutulur ---
    // (vitals ile ayni guvenli primitive; F1'de calistigi kanitlandi)
    if (s.moneyLock) {
        MonoClass* mm = mono.FindClass("", "MoneyManager");
        if (mm) {
            void* minst = mono.GetStaticObject(mm, "Instance");
            if (minst && mono.IsInstanceOf(minst, mm)) {
                if (s.moneyLockValue <= 0) {
                    void* sync0 = ReadObjField(minst, mm, "_money");
                    int cur = 0; // kilit degeri yoksa mevcut degeri benimse
                    if (sync0 && SyncVarReadInt(sync0, cur)) s.moneyLockValue = cur;
                } else {
                    void* sync = ReadObjField(minst, mm, "_money");
                    if (sync) SyncVarWriteInt(sync, s.moneyLockValue);
                    WriteBacking(minst, mm, (uint64_t)s.moneyLockValue);
                }
            }
        }
    }

    // --- Can / aclik (Cecil ile kanitli tipler: int + SyncVar<int>) ---
    // NOT: hicbir Unity metodu cagrilmiyor, sadece ham field yazma.
    if (s.godMode || s.noHunger) {
        void* vit = ReadObjField(lp, pc, "_playerVitals");
        MonoClass* vc = vit ? mono.ObjectClass(vit) : nullptr;
        if (!vc) vc = mono.FindClass("", "PlayerVitals");
        if (!mono.IsInstanceOf(vit, vc)) vit = nullptr;
        if (vit && vc) {
            // dnSpy: _maxHealth const int (100) -> runtime'da okunmaz, hardcode.
            // _maxFullness static okunur, sacmaysa 100 kullanilir.
            int maxH = 100, maxF = 100;
            {
                int tmp = 0;
                if (mono.GetStaticInt(vc, "_maxFullness", tmp) && tmp > 0 && tmp <= 10000) maxF = tmp;
            }
            if (s.godMode) {
                WriteIntChecked(vit, vc, "_localHp", maxH);
                WriteIntChecked(vit, vc, "_prevHealth", maxH);
                SyncVarWriteOnField(vit, vc, "_syncedHealth", maxH);
                SyncVarWriteOnField(vit, vc, "_syncedPoison", 0);
                SyncVarWriteOnField(vit, vc, "_syncedFire", 0);
                WriteIntChecked(vit, vc, "_prevPoison", 0);
                WriteIntChecked(vit, vc, "_prevFire", 0);
            }
            if (s.noHunger || s.godMode) {
                SyncVarWriteOnField(vit, vc, "_syncedFullness", maxF);
                WriteIntChecked(vit, vc, "_prevFullness", maxF);
            }
        }
    }

    // --- Eldeki obje (mermi + silah hasari icin bir kere coz) ---
    void* hold = nullptr;
    MonoClass* hcHold = nullptr;
    if (s.infAmmo || s.noRecoil || g_nrWeapon || s.dmgIndex != 0 || s.projCached) {
        hold = ReadObjField(lp, pc, "_holding");
        if (hold) hcHold = mono.ObjectClass(hold);
    }

    // --- No Recoil: Shoot MinHook calismasa da kalici alan + RET patch yolu ---
    if (s.noRecoil) {
        ClearCameraRecoil(lp, pc);
        if (hold && hcHold) {
            void* wobj = hold; MonoClass* wcls = hcHold;
            ResolveWeapon(hold, hcHold, &wobj, &wcls);
            ApplyNoRecoilWeapon(wobj, wcls);
            ZeroNoRecoilBarrels(wobj, wcls);
        }
    } else if (g_nrWeapon) {
        RestoreNoRecoilWeapon();
        RestoreNoRecoilBarrels();
    } else if (!g_nrBarrels.empty()) {
        RestoreNoRecoilBarrels();
    }

    // --- Mermi (eldeki Weapon: <Ammo>k__BackingField) ---
    // hold PlayerHolding wrapper'sa _heldItem'a inilir (ResolveWeapon)
    if (s.infAmmo && hold && hcHold) {
        void* wobj = hold; MonoClass* wcls = hcHold;
        ResolveWeapon(hold, hcHold, &wobj, &wcls);
        uint32_t off = 0;
        if (IsInt32(wcls, "<Ammo>k__BackingField") && mono.GetFieldOffset(wcls, "<Ammo>k__BackingField", off))
            RawWrite32(wobj, off, 999);
        if (mono.FieldTypeCode(wcls, "_isReloading") == 2 && mono.GetFieldOffset(wcls, "_isReloading", off))
            RawWrite8(wobj, off, 0);
    }

    // --- Hiz (PlayerMovement._walkSpeed/_sprintSpeed, orijinali sakla/geri yaz) ---
    void* mov = nullptr;
    MonoClass* mc = nullptr;
    if (s.speedHack || s.speedCached) {
        mov = ReadObjField(lp, pc, "_movement");
        if (mov) { mc = mono.ObjectClass(mov); if (!mc) mc = mono.FindClass("", "PlayerMovement"); }
        if (!mono.IsInstanceOf(mov, mc)) { mov = nullptr; mc = nullptr; }
    }
    if (s.speedHack && mov && mc) {
        uint32_t off = 0;
        if (!s.speedCached) {
            uint32_t w = 0, sp = 0; bool ok = false;
            if (mono.GetFieldOffset(mc, "_walkSpeed", off) && RawRead32(mov, off, w)) { s.baseWalkRaw = w; ok = true; }
            if (mono.GetFieldOffset(mc, "_sprintSpeed", off) && RawRead32(mov, off, sp)) { s.baseSprintRaw = sp; ok = true; }
            if (ok) { s.speedCached = true; s.Log("[+] Hiz baz degerleri okundu."); }
        }
        if (s.speedCached) {
            float m = s.speedMult;
            if (mono.GetFieldOffset(mc, "_walkSpeed", off)) {
                float b = RawToFloat(s.baseWalkRaw);
                if (b > 0.01f && b < 1000.f) { float nv = b * m; RawWrite32(mov, off, FloatToRaw(nv)); }
            }
            if (mono.GetFieldOffset(mc, "_sprintSpeed", off)) {
                float b = RawToFloat(s.baseSprintRaw);
                if (b > 0.01f && b < 1000.f) { float nv = b * m; RawWrite32(mov, off, FloatToRaw(nv)); }
            }
        }
    } else if (s.speedCached && mov && mc) {
        uint32_t off = 0; // hile kapatildi -> orijinali geri yaz
        if (mono.GetFieldOffset(mc, "_walkSpeed", off)) RawWrite32(mov, off, s.baseWalkRaw);
        if (mono.GetFieldOffset(mc, "_sprintSpeed", off)) RawWrite32(mov, off, s.baseSprintRaw);
        s.speedCached = false;
        s.Log("[-] Hiz normale dondu.");
    }

    // --- Hasar (PlayerPunching._damage + Weapon._weaponInfo.<ProjectileDamage>) ---
    void* punch = nullptr;
    MonoClass* ppc = nullptr;
    if (s.dmgIndex != 0 || s.dmgCached) {
        punch = ReadObjField(lp, pc, "_playerPunching");
        if (punch) { ppc = mono.ObjectClass(punch); if (!ppc) ppc = mono.FindClass("", "PlayerPunching"); }
        if (!mono.IsInstanceOf(punch, ppc)) { punch = nullptr; ppc = nullptr; }
    }
    if (ppc && punch) {
        uint32_t off = 0;
        if (mono.GetFieldOffset(ppc, "_damage", off)) {
            if (!s.dmgCached) {
                if (RawRead32(punch, off, s.basePunchRaw)) s.dmgCached = true;
            }
            if (s.dmgCached) {
                if (s.dmgIndex == 0) {
                    RawWrite32(punch, off, s.basePunchRaw); // geri yaz
                    s.dmgCached = false;
                } else {
                    ApplyDamageMult(punch, off, s.dmgIndex, s.basePunchRaw);
                }
            }
        }
    }
    // Silah mermi hasari: hold -> (_heldItem) -> _weaponInfo -> <ProjectileDamage>k__BackingField
    if ((s.dmgIndex != 0 || s.projCached) && hold && hcHold) {
        void* wobj = hold; MonoClass* wcls = hcHold;
        ResolveWeapon(hold, hcHold, &wobj, &wcls);
        void* wi = ReadObjField(wobj, wcls, "_weaponInfo");
        if (wi) {
            MonoClass* wic = mono.ObjectClass(wi);
            if (!wic) wic = mono.FindClass("", "WeaponInfo");
            if (wic) {
                uint32_t off = 0;
                if (mono.GetFieldOffset(wic, "<ProjectileDamage>k__BackingField", off)) {
                    if (!s.projCached) {
                        if (RawRead32(wi, off, s.baseProjRaw)) s.projCached = true;
                    }
                    if (s.projCached) {
                        if (s.dmgIndex == 0) {
                            RawWrite32(wi, off, s.baseProjRaw);
                            s.projCached = false;
                        } else {
                            ApplyDamageMult(wi, off, s.dmgIndex, s.baseProjRaw);
                        }
                    }
                }
            }
        }
    }
}

// END cikisi: tum hile bayraklarini kapat, yazilan degerleri geri al.
// ~1 sn Tick calistirilir (hiz/hasar orijinaline doner), patchler geri alinir.
void ShutdownCheats() {
    auto& s = State();
    s.Log("[*] Hileler kapatiliyor, oyun normale donuyor...");
    s.godMode = s.noHunger = s.infAmmo = s.airJump = s.speedHack = false;
    s.noRecoil = false;
    SetNoRecoil(false);
    s.dmgIndex = 0;
    s.moneyLock = false;
    s.silentAim = false;
    s.espValid = false;
    s.casinoWin = false;
    s.casinoMult = 1.0f;
    ApplyAmmo(false); // set_Ammo patch geri al (aciksa)
    for (int i = 0; i < 20; ++i) { Tick(); Sleep(50); } // hiz/hasar restore
    s.speedCached = s.dmgCached = s.projCached = false;
    s.Log("[*] Temiz. Oyun normal devam edebilir.");
}

void DumpDiagnostics() {
    auto& s = State();
    s.Log("== Tani dokumu (tam log icin yukari kaydir) ==");
    const char* klasses[] = { "PlayerVitals", "Weapon", "MoneyManager", "Inventory",
        "Player", "PlayerUI", "MoneyUI", "PlayerMovement", "PlayerPunching",
        "WeaponInfo", "Melee", "Attachments", "PlayerInventory", "PlayerHolding",
        "Creature", "Fishable", "GameInfo" };
    for (auto* k : klasses) DumpClassFull(k);
    // _inventory / _holding alanlarinin GERCEK runtime siniflari (Inventory yoksa adini buluruz)
    auto& mono = MonoAPI::Get();
    void* lp = GetLocalPlayer();
    if (!lp) {
        s.Log(">> LocalPlayer null (lobiye girip 'Tani Dokumu'na tekrar bas).");
        return;
    }
    MonoClass* pc = mono.FindClass("", "Player");
    if (!pc) return;
    void* inv = ReadObjField(lp, pc, "_inventory");
    if (inv) {
        MonoClass* ic = mono.ObjectClass(inv);
        std::string cn = ic ? mono.ClassName(ic) : "?";
        s.Log(std::string(">> _inventory gercek sinif: ") + cn);
        if (ic) DumpClassFull(cn.c_str());
    }
    void* hold = ReadObjField(lp, pc, "_holding");
    if (hold) {
        MonoClass* hc = mono.ObjectClass(hold);
        std::string cn = hc ? mono.ClassName(hc) : "?";
        s.Log(std::string(">> _holding gercek sinif: ") + cn);
    }
}

} // namespace cheats
