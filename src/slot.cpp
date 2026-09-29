// Slot makinesi rig gerceklemesi. Bkz. slot.h
// Akis (Cecil-kanitli): GameInfo.GetSpawnable -> Item.SkinPreset ->
// Skins[] tara (Rarity.Legendary=3) -> SetCheatSkin(item, idx) ->
// RollRandom(LocalPlayer). Tamami main thread'de.
#include "slot.h"
#include "cheats.h"
#include "mono_api.h"
#include "memory.h"
#include <windows.h>
#include <mutex>
#include <string>
#include <algorithm>
#include <cctype>

namespace slot {
static std::string g_fish;
static std::mutex g_mtx;
static volatile LONG g_pending = 0;
static volatile LONG g_dumpPending = 0;

static bool RawRead32(void* o, uint32_t off, uint32_t& v);

static const char* RarityName(int r) {
    switch (r) {
        case 0: return "Default";
        case 1: return "Common";
        case 2: return "Rare";
        case 3: return "Legendary";
        default: return "?";
    }
}

// SlotMachine odul havuzu: AvailableItems (static property, yoksa instance dene)
static void* CasinoPrizeArray() {
    auto& mono = MonoAPI::Get();
    MonoClass* sm = mono.FindClass("", "SlotMachine");
    if (!sm) sm = mono.FindClass("", "SlotMachineManager");
    if (!sm) return nullptr;
    MonoMethod* ga = mono.FindMethod(sm, "get_AvailableItems", 0);
    if (!ga) ga = mono.FindMethodAny(sm, "AvailableItems");
    if (!ga) return nullptr;
    // static mi instance mi? once static dene
    void* arr = mono.Invoke(ga, nullptr, nullptr);
    if (arr) return arr;
    // instance yedegi: SlotMachineManager._instance
    MonoClass* smm = mono.FindClass("", "SlotMachineManager");
    void* inst = smm ? mono.GetStaticObject(smm, "_instance") : nullptr;
    if (inst) arr = mono.Invoke(ga, inst, nullptr);
    return arr;
}

static std::string ObjName(void* obj) {
    auto& mono = MonoAPI::Get();
    if (!obj) return "";
    MonoClass* oc = mono.ObjectClass(obj);
    if (!oc) return "";
    MonoMethod* nm = mono.FindMethod(oc, "get_name", 0);
    if (!nm) {
        MonoClass* uo = mono.FindClass("UnityEngine", "Object");
        if (uo) nm = mono.FindMethod(uo, "get_name", 0);
    }
    if (!nm) return "";
    return mono.StringUtf8(mono.Invoke(nm, obj, nullptr));
}


static std::string Normalized(std::string s) {
    s.erase(std::remove_if(s.begin(), s.end(), [](unsigned char c) {
        return c == ' ' || c == '_' || c == '-';
    }), s.end());
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static int ListCount(void* list) {
    auto& mono = MonoAPI::Get();
    MonoClass* c = mono.ObjectClass(list);
    MonoMethod* m = c ? mono.FindMethod(c, "get_Count", 0) : nullptr;
    return m ? mono.UnboxInt(mono.Invoke(m, list, nullptr)) : 0;
}

// Dizi (Array) icin: get_Length (get_Count dizilerde YOKTUR!)
static int ArrCount(void* arr) {
    auto& mono = MonoAPI::Get();
    MonoClass* c = mono.ObjectClass(arr);
    if (!c) return -1;
    MonoMethod* m = mono.FindMethod(c, "get_Length", 0);
    if (!m) m = mono.FindMethod(c, "get_Count", 0);
    if (!m) return -1;
    void* b = mono.Invoke(m, arr, nullptr);
    int* pi = (int*)mono.UnboxPtr(b);
    return pi ? *pi : -1;
}

// Dizi elemani: GetValue(int)
static void* ArrItem(void* arr, int index) {
    auto& mono = MonoAPI::Get();
    MonoClass* c = mono.ObjectClass(arr);
    if (!c) return nullptr;
    MonoMethod* m = mono.FindMethod(c, "GetValue", 1);
    if (!m) return nullptr;
    void* args[1] = { &index };
    return mono.Invoke(m, arr, args);
}

static void* ListItem(void* list, int index) {
    auto& mono = MonoAPI::Get();
    MonoClass* c = mono.ObjectClass(list);
    MonoMethod* m = c ? mono.FindMethod(c, "GetItem", 1) : nullptr;
    // get_Item de olabilir
    if (!m && c) {
        mono.ForEachMethod(c, [&](MonoMethod* mm) {
            if (m) return;
            const char* n = mono.MethodName(mm);
            if (!n || strcmp(n, "get_Item") != 0) return;
            if (mono.ParamCount(mm) != 1) return;
            m = mm;
        });
    }
    if (!m) return nullptr;
    void* args[1] = { &index };
    return mono.Invoke(m, list, args);
}

static void* LocalPlayer() {
    auto& mono = MonoAPI::Get();
    MonoClass* pc = mono.FindClass("", "Player");
    if (!pc) return nullptr;
    const char* names[] = { "LocalPlayer", "<LocalPlayer>k__BackingField",
        "_localPlayer", "localPlayer", "Instance", "_instance", "Local" };
    for (auto* n : names) {
        void* o = mono.GetStaticObject(pc, n);
        if (o && mono.IsInstanceOf(o, pc)) return o;
    }
    return nullptr;
}

// Havuzda isme gore Item bul (bosluk/kucuk harf duyarsiz)
static void* FindCasinoItem(const char* wantNorm) {
    auto& mono = MonoAPI::Get();
    void* arr = CasinoPrizeArray();
    if (!arr) return nullptr;
    int n = ArrCount(arr);
    if (n <= 0 || n > 512) return nullptr;
    for (int i = 0; i < n && i < 200; ++i) {
        void* item = ArrItem(arr, i);
        if (!item) continue;
        std::string iname = ObjName(item);
        if (iname.empty()) continue;
        std::string norm;
        for (const char* p = iname.c_str(); *p; ++p) {
            if (*p == ' ' || *p == '_' || *p == '-') continue;
            norm += (char)tolower((unsigned char)*p);
        }
        if (norm.find(wantNorm) != std::string::npos || strstr(wantNorm, norm.c_str()) != nullptr)
            return item;
    }
    return nullptr;
}

// Tum odul havuzunu dok: isim + skin rarity'leri
static void DumpPrizes() {
    auto& s = State();
    auto& mono = MonoAPI::Get();
    void* arr = CasinoPrizeArray();
    if (!arr) { s.Log("[!] Odul havuzu bulunamadi (SlotMachine.AvailableItems)."); return; }
    int n = ArrCount(arr);
    if (n < 0) { s.Log("[!] Odul dizisi okunamadi."); return; }
    if (n == 0) {
        s.Log("[!] Odul havuzu BOS (0 item). Casino sahnesi yuklu degilse casinonun oldugu adaya git ve tekrar dene.");
        return;
    }
    if (n > 512) {
        char b[128];
        sprintf_s(b, "[!] Odul sayisi garip: %d", n);
        s.Log(b);
        return;
    }
    char hb[128];
    sprintf_s(hb, "=== Casino odul havuzu: %d item ===", n);
    s.Log(hb);
    for (int i = 0; i < n && i < 120; ++i) {
        void* item = ArrItem(arr, i);
        if (!item) { s.Log("  [BOAT]"); continue; }
        std::string iname = ObjName(item);
        std::string skins;
        {
            MonoClass* ic = mono.ObjectClass(item);
            MonoMethod* gsp = ic ? mono.FindMethod(ic, "get_SkinPreset", 0) : nullptr;
            void* preset = gsp ? mono.Invoke(gsp, item, nullptr) : nullptr;
            MonoClass* pc2 = preset ? mono.ObjectClass(preset) : nullptr;
            MonoMethod* gsl = pc2 ? mono.FindMethod(pc2, "get_Skins", 0) : nullptr;
            void* sks = gsl ? mono.Invoke(gsl, preset, nullptr) : nullptr;
            int sn = sks ? ListCount(sks) : 0;
            if (sn <= 0 || sn > 64) skins = "SkinPreset=NULL";
            else {
                char sb[256];
                sprintf_s(sb, "skin=%d[", sn > 12 ? 12 : sn);
                skins = sb;
                for (int k = 0; k < sn && k < 12; ++k) {
                    void* sk = ListItem(sks, k);
                    int rv = -1;
                    if (sk) {
                        MonoClass* skc = mono.ObjectClass(sk);
                        MonoClassField* rf = skc ? mono.GetField(skc, "_rarity") : nullptr;
                        uint32_t roff = 0;
                        if (rf && mono.FieldOffsetOf(rf, roff)) {
                            void* data = mono.UnboxPtr(sk);
                            uint32_t raw = 0;
                            if (data && RawRead32(data, roff, raw)) rv = (int)raw;
                        }
                    }
                    char tb[32];
                    sprintf_s(tb, "%d:%s ", k, RarityName(rv));
                    skins += tb;
                }
                skins += "]";
            }
        }
        s.Log(std::string("  ") + iname + " | " + skins);
    }
}

enum SlotStep {
    SLOT_OK = 0,
    SLOT_NO_GAMEINFO, SLOT_NO_METHOD, SLOT_NO_PREFAB,
    SLOT_NO_PRESET, SLOT_NO_SKINS, SLOT_BAD_COUNT,
    SLOT_NO_LEGENDARY, SLOT_NO_MANAGER, SLOT_NO_ROLLMETHODS,
    SLOT_NO_PLAYER, SLOT_EXCEPT
};
struct SlotOutcome { int code = -1; int found = -1; char info[96] = { 0 }; };

static bool RawRead32(void* o, uint32_t off, uint32_t& v) {
    __try { v = *(uint32_t*)((uint8_t*)o + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Lambda+__try birlesimi C2712 verir: tarama ayri fonksiyonda (try'siz)
static MonoMethod* FindGetSpawnable(MonoClass* gi) {
    auto& mono = MonoAPI::Get();
    MonoMethod* gs = nullptr;
    mono.ForEachMethod(gi, [&](MonoMethod* m) {
        if (gs) return;
        const char* n = mono.MethodName(m);
        if (!n || strcmp(n, "GetSpawnable") != 0) return;
        if (mono.ParamCount(m) != 1) return;
        if (mono.ParamTypeCode(m, 0) != 14) return; // string
        gs = m;
    });
    return gs;
}

void Request(const char* fishName) {
    if (!fishName || !*fishName) return;
    std::lock_guard<std::mutex> lock(g_mtx);
    g_fish = fishName;
    InterlockedExchange(&g_pending, 1);
    State().Log(std::string("[*] Slot kuyrukta: ") + fishName);
}

static void TickInner(const char* want, SlotOutcome& o);

void DumpRequest() { InterlockedExchange(&g_dumpPending, 1); }

void Tick() {
    if (InterlockedCompareExchange(&g_dumpPending, 0, 0)) {
        InterlockedExchange(&g_dumpPending, 0);
        DumpPrizes();
    }
    if (!InterlockedCompareExchange(&g_pending, 0, 0)) return;
    InterlockedExchange(&g_pending, 0);
    // std::string __try icinde yasak (C2712): C buffer'a tasi
    char fish[64] = { 0 };
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        strncpy_s(fish, g_fish.c_str(), _TRUNCATE);
    }
    auto& mono = MonoAPI::Get();
    auto& s = State();
    if (!fish[0]) return;
    char want[64] = { 0 };
    {
        int w = 0;
        for (const char* p = fish; *p && w < 60; ++p) {
            if (*p == ' ' || *p == '_' || *p == '-') continue;
            want[w++] = (char)tolower((unsigned char)*p);
        }
    }
    if (!want[0]) return;
    SlotOutcome o;
    TickInner(want, o);
    switch (o.code) {
        case SLOT_OK: {
            char fb[128];
            sprintf_s(fb, "[+] Slot cevriliyor (Legendary force, index %d).", o.found);
            s.Log(fb);
            break;
        }
        case SLOT_NO_GAMEINFO: s.Log("[!] Slot: GameInfo yok."); break;
        case SLOT_NO_METHOD: s.Log("[!] Slot: GetSpawnable(string) yok."); break;
        case SLOT_NO_PREFAB: s.Log(std::string("[!] Slot: prefab yok: ") + want); break;
        case SLOT_NO_PRESET: s.Log("[!] Slot: SkinPreset yok."); break;
        case SLOT_NO_SKINS: s.Log("[!] Slot: Skins yok."); break;
        case SLOT_BAD_COUNT: s.Log("[!] Slot: skin sayisi garip."); break;
        case SLOT_NO_LEGENDARY: s.Log("[!] Slot: Legendary skin yok."); break;
        case SLOT_NO_MANAGER: s.Log("[!] Slot: SlotMachineManager yok."); break;
        case SLOT_NO_ROLLMETHODS: s.Log("[!] Slot: SetCheatSkin/RollRandom yok."); break;
        case SLOT_NO_PLAYER: s.Log("[!] Slot: LocalPlayer yok."); break;
        default: s.Log("[!] Slot sirasinda hata."); break;
    }
}

static void TickInner(const char* want, SlotOutcome& o) {
    auto& mono = MonoAPI::Get();
    o.code = SLOT_EXCEPT;
    o.found = -1;
    o.info[0] = 0;

    __try {
        // 0) Casino odul havuzu (gercek oduller burada)
        void* item = FindCasinoItem(want);
        if (!item) {
            // 1) Yedek: GameInfo.GetSpawnable(string)
            MonoClass* gi = mono.FindClass("", "GameInfo");
            if (!gi) { o.code = SLOT_NO_GAMEINFO; return; }
            MonoMethod* gs = FindGetSpawnable(gi);
            if (!gs) { o.code = SLOT_NO_METHOD; return; }
            void* ks = mono.NewString(want);
            if (!ks) return;
            void* ga[1] = { ks };
            item = mono.Invoke(gs, nullptr, ga);
            if (!item) { o.code = SLOT_NO_PREFAB; return; }
        }
        // 2) SkinPreset -> Skins
        MonoClass* ic = mono.ObjectClass(item);
        if (!ic) return;
        MonoMethod* gsp = mono.FindMethod(ic, "get_SkinPreset", 0);
        if (!gsp) { o.code = SLOT_NO_PRESET; return; }
        void* preset = mono.Invoke(gsp, item, nullptr);
        if (!preset) { o.code = SLOT_NO_PRESET; return; }
        MonoClass* pc2 = mono.ObjectClass(preset);
        if (!pc2) return;
        MonoMethod* gsl = mono.FindMethod(pc2, "get_Skins", 0);
        if (!gsl) { o.code = SLOT_NO_SKINS; return; }
        void* skins = mono.Invoke(gsl, preset, nullptr);
        if (!skins) return;
        int n = ListCount(skins);
        if (n <= 0 || n > 128) { o.code = SLOT_BAD_COUNT; return; }
        // 3) Legendary ara (0=Default 1=Common 2=Rare 3=Legendary)
        int found = -1;
        for (int i = 0; i < n; ++i) {
            void* sk = ListItem(skins, i);
            if (!sk) continue;
            MonoClass* skc = mono.ObjectClass(sk);
            if (!skc) continue;
            MonoClassField* rf = mono.GetField(skc, "_rarity");
            uint32_t roff = 0;
            int rv = -1;
            if (rf && mono.FieldOffsetOf(rf, roff)) {
                void* data = mono.UnboxPtr(sk);
                if (data) {
                    uint32_t raw = 0;
                    if (RawRead32(data, roff, raw)) rv = (int)raw;
                }
            }
            if (rv == 3) { found = i; break; }
        }
        if (found < 0) { o.code = SLOT_NO_LEGENDARY; return; }
        o.found = found;
        // 4) SetCheatSkin(item, idx) + RollRandom(LocalPlayer)
        MonoClass* smc = mono.FindClass("", "SlotMachineManager");
        if (!smc) { o.code = SLOT_NO_MANAGER; return; }
        MonoMethod* scs = mono.FindMethod(smc, "SetCheatSkin", 2);
        MonoMethod* roll = mono.FindMethod(smc, "RollRandom", 1);
        if (!scs || !roll) { o.code = SLOT_NO_ROLLMETHODS; return; }
        uint8_t idx = (uint8_t)found;
        void* sa[2] = { item, &idx };
        mono.Invoke(scs, nullptr, sa);
        void* lp = LocalPlayer();
        if (!lp) { o.code = SLOT_NO_PLAYER; return; }
        void* ra[1] = { lp };
        mono.Invoke(roll, nullptr, ra);
        o.code = SLOT_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        o.code = SLOT_EXCEPT;
    }
}

// --- Casino debug (RollRandom force) icin paylasilan okuyucular ---
int SkinRarityOf(void* itemObj, int skinIndex) {
    if (!itemObj || skinIndex < 0 || skinIndex > 64) return -1;
    auto& mono = MonoAPI::Get();
    int rv = -1;
    __try {
        MonoClass* ic = mono.ObjectClass(itemObj);
        if (!ic) return -1;
        MonoMethod* gsp = mono.FindMethod(ic, "get_SkinPreset", 0);
        if (!gsp) return -1;
        void* preset = mono.Invoke(gsp, itemObj, nullptr);
        if (!preset) return -1;
        MonoClass* pc2 = mono.ObjectClass(preset);
        if (!pc2) return -1;
        MonoMethod* gsl = mono.FindMethod(pc2, "get_Skins", 0);
        if (!gsl) return -1;
        void* skins = mono.Invoke(gsl, preset, nullptr);
        if (!skins) return -1;
        int n = ListCount(skins);
        if (skinIndex >= n) return -1;
        void* sk = ListItem(skins, skinIndex);
        if (!sk) return -1;
        MonoClass* skc = mono.ObjectClass(sk);
        if (!skc) return -1;
        MonoClassField* rf = mono.GetField(skc, "_rarity");
        uint32_t roff = 0;
        if (!rf || !mono.FieldOffsetOf(rf, roff)) return -1;
        void* data = mono.UnboxPtr(sk);
        if (!data) return -1;
        uint32_t raw = 0;
        if (!RawRead32(data, roff, raw)) return -1;
        rv = (int)raw;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return rv;
}

void ItemNameOf(void* itemObj, char* out, size_t cap) {
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!itemObj) return;
    // ObjName icindeki tum mono cagrilari zaten SEH korumali, burada __try yok (C2712).
    std::string n = ObjName(itemObj);
    strncpy_s(out, cap, n.c_str(), _TRUNCATE);
}
} // namespace slot
