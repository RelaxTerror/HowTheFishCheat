#include "fishspawn.h"
#include "mono_api.h"
#include "cheats.h"
#include <windows.h>
#include <mutex>
#include <string>
#include <algorithm>
#include <cctype>

namespace fishspawn {
struct Request {
    std::string name;
    int count = 0;
    bool dead = true;
    bool drip = true;
    bool pending = false;
};
static Request g_req;
static std::mutex g_mtx;

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

static void* ListItem(void* list, int index) {
    auto& mono = MonoAPI::Get();
    MonoClass* c = mono.ObjectClass(list);
    MonoMethod* m = c ? mono.FindMethod(c, "get_Item", 1) : nullptr;
    void* args[1] = { &index };
    return m ? mono.Invoke(m, list, args) : nullptr;
}

// Oyunun gercek balik secim zinciri:
// GameInfo.AllBaits -> BaitInfo.ItemWeights -> Fishable.ItemToSpawn (Item prefab).
static bool ResolveFromFishables(const std::string& requested, std::string& exact, void** outPrefab = nullptr) {
    if (outPrefab) *outPrefab = nullptr;
    auto& mono = MonoAPI::Get();
    MonoClass* gi = mono.FindClass("", "GameInfo");
    MonoClass* baitInfo = mono.FindClass("", "BaitInfo");
    MonoClass* weight = mono.FindClass("", "ItemInfoWeight");
    MonoClass* fishable = mono.FindClass("", "Fishable");
    MonoClass* item = mono.FindClass("", "Item");
    MonoClass* uo = mono.FindClass("UnityEngine", "Object");
    if (!gi || !baitInfo || !weight || !fishable || !item || !uo) return false;
    MonoMethod* allBaitsM = mono.FindMethod(gi, "get_AllBaits", 0);
    MonoMethod* weightsM = mono.FindMethod(baitInfo, "get_ItemWeights", 0);
    MonoMethod* fishableM = mono.FindMethod(weight, "get_Fishable", 0);
    MonoMethod* itemM = mono.FindMethod(fishable, "get_ItemToSpawn", 0);
    MonoMethod* creatureM = mono.FindMethod(item, "get_Creature", 0);
    MonoMethod* nameM = mono.FindMethod(uo, "get_name", 0);
    if (!allBaitsM || !weightsM || !fishableM || !itemM || !creatureM || !nameM) return false;
    void* baits = mono.Invoke(allBaitsM, nullptr, nullptr);
    int baitCount = ListCount(baits);
    if (baitCount <= 0 || baitCount > 256) return false;
    std::string want = Normalized(requested);
    std::string partial;
    void* partialObj = nullptr;
    for (int b = 0; b < baitCount; ++b) {
        void* bait = ListItem(baits, b);
        void* weights = bait ? mono.Invoke(weightsM, bait, nullptr) : nullptr;
        int weightCount = ListCount(weights);
        if (weightCount <= 0 || weightCount > 512) continue;
        for (int w = 0; w < weightCount; ++w) {
            void* entry = ListItem(weights, w);
            void* fish = entry ? mono.Invoke(fishableM, entry, nullptr) : nullptr;
            void* spawnItem = fish ? mono.Invoke(itemM, fish, nullptr) : nullptr;
            if (!spawnItem) continue;
            std::string itemName = mono.StringUtf8(mono.Invoke(nameM, spawnItem, nullptr));
            void* creature = mono.Invoke(creatureM, spawnItem, nullptr);
            std::string creatureName = creature ? mono.StringUtf8(mono.Invoke(nameM, creature, nullptr)) : "";
            std::string itemNorm = Normalized(itemName);
            std::string creatureNorm = Normalized(creatureName);
            bool exactMatch = itemNorm == want || creatureNorm == want;
            bool partialMatch = itemNorm.find(want) != std::string::npos || creatureNorm.find(want) != std::string::npos ||
                                want.find(itemNorm) != std::string::npos || (!creatureNorm.empty() && want.find(creatureNorm) != std::string::npos);
            if (exactMatch && !itemName.empty()) { exact = itemName; if (outPrefab) *outPrefab = spawnItem; return true; }
            if (partial.empty() && partialMatch && !itemName.empty()) { partial = itemName; partialObj = spawnItem; }
        }
    }
    if (!partial.empty()) { exact = partial; if (outPrefab) *outPrefab = partialObj; return true; }
    return false;
}

static bool ResolvePrefabName(const std::string& requested, std::string& exact, void** outPrefab = nullptr) {
    if (outPrefab) *outPrefab = nullptr;
    if (ResolveFromFishables(requested, exact)) return true;
    // Eski/ozel baliklar bait tablosunda yoksa ID tabanli yol yedek kalir.
    auto& mono = MonoAPI::Get();
    MonoClass* gi = mono.FindClass("", "GameInfo");
    MonoClass* creatureClass = mono.FindClass("", "Creature");
    MonoClass* uo = mono.FindClass("UnityEngine", "Object");
    if (!gi || !creatureClass || !uo) return false;
    MonoMethod* countM = mono.FindMethod(gi, "get_AllCreatureCount", 0);
    MonoMethod* getM = mono.FindMethod(gi, "GetCreature", 1);
    MonoMethod* idM = mono.FindMethod(creatureClass, "get_ID", 0);
    MonoMethod* nameM = mono.FindMethod(uo, "get_name", 0);
    MonoMethod* spawnByIdM = nullptr;
    mono.ForEachMethod(gi, [&](MonoMethod* m) {
        if (spawnByIdM || _stricmp(mono.MethodName(m), "GetSpawnable") != 0) return;
        if (mono.ParamCount(m) == 1 && mono.ParamTypeCode(m, 0) == 5) spawnByIdM = m; // byte / U1
    });
    if (!countM || !getM || !idM || !nameM || !spawnByIdM) return false;
    int count = mono.UnboxInt(mono.Invoke(countM, nullptr, nullptr));
    if (count <= 0 || count > 1000) return false;
    std::string want = Normalized(requested);
    std::string partial;
    void* partialObj = nullptr;
    for (int i = 0; i < count; ++i) {
        void* args[1] = { &i };
        void* creature = mono.Invoke(getM, nullptr, args);
        if (!creature) continue;
        std::string creatureName = mono.StringUtf8(mono.Invoke(nameM, creature, nullptr));
        if (creatureName.empty()) continue;
        std::string norm = Normalized(creatureName);
        bool match = norm == want || norm.find(want) != std::string::npos || want.find(norm) != std::string::npos;
        if (!match) continue;

        void* boxedId = mono.Invoke(idM, creature, nullptr);
        uint8_t* idPtr = (uint8_t*)mono.UnboxPtr(boxedId);
        if (!idPtr) continue;
        uint8_t id = *idPtr;
        void* idArgs[1] = { &id };
        void* spawnableItem = mono.Invoke(spawnByIdM, nullptr, idArgs);
        if (!spawnableItem) continue;
        std::string itemName = mono.StringUtf8(mono.Invoke(nameM, spawnableItem, nullptr));
        if (itemName.empty()) continue;
        if (norm == want) { exact = itemName; if (outPrefab) *outPrefab = spawnableItem; return true; }
        if (partial.empty()) { partial = itemName; partialObj = spawnableItem; }
    }
    if (!partial.empty()) { exact = partial; if (outPrefab) *outPrefab = partialObj; return true; }
    return false;
}

static bool ServerReady() {
    auto& mono = MonoAPI::Get();
    MonoClass* sc = mono.FindClass("", "Server");
    if (!sc) return false;
    MonoMethod* instM = mono.FindMethod(sc, "get_Instance", 0);
    void* server = instM ? mono.Invoke(instM, nullptr, nullptr) : nullptr;
    if (!server) return false;
    MonoClass* rc = mono.ObjectClass(server);
    MonoMethod* readyM = rc ? mono.FindMethod(rc, "get_IsServerInitialized", 0) : nullptr;
    if (!readyM) return true; // Instance varsa eski FishNet surumunde yeterli.
    void* boxed = mono.Invoke(readyM, server, nullptr);
    uint8_t* ready = (uint8_t*)mono.UnboxPtr(boxed);
    return ready && *ready != 0;
}

void Queue(const char* spawnName, int count, bool dead, bool drip) {
    if (!spawnName || !*spawnName) return;
    if (count < 1) count = 1;
    if (count > 10) count = 10;
    std::lock_guard<std::mutex> lock(g_mtx);
    g_req.name = spawnName;
    g_req.count = count;
    g_req.dead = dead;
    g_req.drip = drip;
    g_req.pending = true;
}

struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };

// Basit LCG (tek thread kullanir: Tick main thread'de)
static float RandRange(float a, float b) {
    static uint32_t s = 123456789u;
    s = s * 1664525u + 1013904223u;
    float t = (float)(s >> 8) / 16777216.f;
    return a + t * (b - a);
}

static bool UnVec(void* boxed, Vec3& o) {
    void* p = MonoAPI::Get().UnboxPtr(boxed);
    if (!p) return false;
    __try { o = *(Vec3*)p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool RPtr(void* o, uint32_t off, void*& p) {
    __try { p = *(void**)((uint8_t*)o + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Oyuncu konumu + ilerisi (spawn noktasi icin)
static bool PlayerPose(Vec3& pos, Vec3& fwd) {
    auto& mono = MonoAPI::Get();
    MonoClass* pc = mono.FindClass("", "Player");
    if (!pc) return false;
    const char* names[] = { "LocalPlayer", "<LocalPlayer>k__BackingField",
        "_localPlayer", "localPlayer", "Instance", "_instance", "Local" };
    void* lp = nullptr;
    for (auto* n : names) {
        void* o = mono.GetStaticObject(pc, n);
        if (o && mono.IsInstanceOf(o, pc)) { lp = o; break; }
    }
    if (!lp) return false;
    MonoClass* tc = mono.FindClass("UnityEngine", "Transform");
    if (!tc) return false;
    void* tr = nullptr;
    {
        uint32_t off = 0;
        if (mono.GetFieldOffset(pc, "_transform", off)) RPtr(lp, off, tr);
    }
    if (!tr) {
        MonoMethod* gt = mono.FindMethod(pc, "get_Transform", 0);
        if (!gt) return false;
        tr = mono.Invoke(gt, lp, nullptr);
        if (!tr) return false;
    }
    MonoMethod* gp = mono.FindMethod(tc, "get_position", 0);
    MonoMethod* gf = mono.FindMethod(tc, "get_forward", 0);
    if (!gp || !gf) return false;
    return UnVec(mono.Invoke(gp, tr, nullptr), pos) && UnVec(mono.Invoke(gf, tr, nullptr), fwd);
}

// Object.Instantiate(Object, Vector3, Quaternion) non-generic
static MonoMethod* FindInstantiate() {
    auto& mono = MonoAPI::Get();
    MonoClass* uo = mono.FindClass("UnityEngine", "Object");
    if (!uo) return nullptr;
    MonoMethod* found = nullptr;
    mono.ForEachMethod(uo, [&](MonoMethod* m) {
        if (found) return;
        const char* n = mono.MethodName(m);
        if (!n || strcmp(n, "Instantiate") != 0) return;
        if (mono.ParamCount(m) != 3) return;
        if (mono.ParamTypeCode(m, 1) != 17) return; // Vector3
        if (mono.ParamTypeCode(m, 2) != 17) return; // Quaternion
        found = m;
    });
    return found;
}

// FishNet resmi server spawn: ServerManager.Spawn(GameObject, null, default)
static bool ServerSpawn(void* go) {
    auto& mono = MonoAPI::Get();
    MonoClass* ifc = mono.FindClass("", "InstanceFinder");
    if (!ifc) ifc = mono.FindClass("FishNet", "InstanceFinder");
    if (!ifc) return false;
    MonoMethod* gsm = mono.FindMethod(ifc, "get_ServerManager", 0);
    if (!gsm) return false;
    void* sm = mono.Invoke(gsm, nullptr, nullptr);
    if (!sm) return false;
    MonoClass* sc = mono.ObjectClass(sm);
    if (!sc) return false;
    MonoMethod* sp = nullptr;
    mono.ForEachMethod(sc, [&](MonoMethod* m) {
        if (sp) return;
        const char* n = mono.MethodName(m);
        if (!n || strcmp(n, "Spawn") != 0) return;
        if (mono.ParamCount(m) != 3) return;
        sp = m;
    });
    if (!sp) return false;
    int zeroScene = 0; // default(Scene)
    void* args[3] = { go, nullptr, &zeroScene };
    return mono.InvokeChecked(sp, sm, args);
}

// Birincil prefab yolu: oyunun kendi kaydi.
// GameInfo.GetSpawnable(name) -> Item prefab. Key formati: kucuk harf, bosluksuz
// (ornegin "cod", "triggerfish"). _nameToSpawnable Awake'te doldurulur.
// DIKKAT: GetSpawnable(string) + GetSpawnable(byte) overload'lari var;
// string olani secmek ZORUNLU (byte olani string argumanla patlar).
static std::string LowerStr(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// _nameToSpawnable icinde "cod" gecenleri dok (anahtar + item adi).
// Debug build gerektirmez: ayni isi trainer yapar.
static void DumpCodMatches(MonoClass* gi) {
    auto& s = State();
    auto& mono = MonoAPI::Get();
    void* dict = mono.GetStaticObject(gi, "_nameToSpawnable");
    if (!dict) { s.Log("[!] _nameToSpawnable null."); return; }
    MonoClass* dc = mono.ObjectClass(dict);
    if (!dc) return;
    MonoMethod* ge = mono.FindMethod(dc, "GetEnumerator", 0);
    if (!ge) { s.Log("[!] enumerator yok."); return; }
    void* en = mono.Invoke(ge, dict, nullptr);
    if (!en) return;
    MonoClass* ec = mono.ObjectClass(en);
    if (!ec) return;
    MonoMethod* move = mono.FindMethod(ec, "MoveNext", 0);
    MonoMethod* curM = mono.FindMethod(ec, "get_Current", 0);
    if (!move || !curM) return;
    MonoClass* uo = mono.FindClass("UnityEngine", "Object");
    MonoMethod* nm = uo ? mono.FindMethod(uo, "get_name", 0) : nullptr;
    int total = 0, shown = 0;
    for (int i = 0; i < 400; ++i) {
        void* b = mono.Invoke(move, en, nullptr);
        bool* pb = (bool*)mono.UnboxPtr(b);
        if (!pb || !*pb) break;
        ++total;
        void* kv = mono.Invoke(curM, en, nullptr);
        if (!kv) continue;
        MonoClass* kc = mono.ObjectClass(kv);
        if (!kc) continue;
        MonoMethod* gk = mono.FindMethod(kc, "get_Key", 0);
        MonoMethod* gv = mono.FindMethod(kc, "get_Value", 0);
        if (!gk || !gv) continue;
        std::string key = mono.StringUtf8(mono.Invoke(gk, kv, nullptr));
        void* vs = mono.Invoke(gv, kv, nullptr);
        std::string item = (vs && nm) ? mono.StringUtf8(mono.Invoke(nm, vs, nullptr)) : "";
        if (LowerStr(key).find("cod") != std::string::npos ||
            LowerStr(item).find("cod") != std::string::npos) {
            s.Log(std::string("COD MATCH -> KEY='") + key + "' ITEM='" + item + "'");
            if (++shown >= 10) break;
        }
    }
    char tb[128];
    sprintf_s(tb, ">> taranan kayit: %d, cod eslesme: %d", total, shown);
    s.Log(tb);
}

// _nameToSpawnable TAMAMI: tum key'leri dok (85 kayit civari).
// Debug build gerektirmez.
static void DumpAllSpawnables(MonoClass* gi) {
    auto& s = State();
    auto& mono = MonoAPI::Get();
    void* dict = mono.GetStaticObject(gi, "_nameToSpawnable");
    if (!dict) { s.Log("[!] _nameToSpawnable null."); return; }
    MonoClass* dc = mono.ObjectClass(dict);
    if (!dc) return;
    MonoMethod* ge = mono.FindMethod(dc, "GetEnumerator", 0);
    if (!ge) { s.Log("[!] enumerator yok."); return; }
    void* en = mono.Invoke(ge, dict, nullptr);
    if (!en) return;
    MonoClass* ec = mono.ObjectClass(en);
    if (!ec) return;
    MonoMethod* move = mono.FindMethod(ec, "MoveNext", 0);
    MonoMethod* curM = mono.FindMethod(ec, "get_Current", 0);
    if (!move || !curM) return;
    MonoClass* uo = mono.FindClass("UnityEngine", "Object");
    MonoMethod* nm = uo ? mono.FindMethod(uo, "get_name", 0) : nullptr;
    s.Log("=== SPAWNABLE DUMP ===");
    int total = 0;
    for (int i = 0; i < 200; ++i) {
        void* b = mono.Invoke(move, en, nullptr);
        bool* pb = (bool*)mono.UnboxPtr(b);
        if (!pb || !*pb) break;
        ++total;
        void* kv = mono.Invoke(curM, en, nullptr);
        if (!kv) continue;
        MonoClass* kc = mono.ObjectClass(kv);
        if (!kc) continue;
        MonoMethod* gk = mono.FindMethod(kc, "get_Key", 0);
        MonoMethod* gv = mono.FindMethod(kc, "get_Value", 0);
        if (!gk || !gv) continue;
        std::string key = mono.StringUtf8(mono.Invoke(gk, kv, nullptr));
        void* vs = mono.Invoke(gv, kv, nullptr);
        std::string item;
        if (vs && nm) {
            // Value Item olmayabilir; get_name Component uzerinden de cozulur
            MonoClass* vc = mono.ObjectClass(vs);
            MonoMethod* nn = vc ? mono.FindMethod(vc, "get_name", 0) : nullptr;
            if (!nn) nn = nm;
            item = mono.StringUtf8(mono.Invoke(nn, vs, nullptr));
        }
        if (item.empty()) item = "NULL";
        s.Log(std::string("SPAWNABLE KEY=[") + key + "] VALUE=[" + item + "]");
    }
    char tb[128];
    sprintf_s(tb, "=== toplam %d kayit ===", total);
    s.Log(tb);
}

static void* GetSpawnablePrefab(const std::string& requested) {
    auto& mono = MonoAPI::Get();
    MonoClass* gi = mono.FindClass("", "GameInfo");
    if (!gi) { State().Log("[!] GetSpawnable: GameInfo yok."); return nullptr; }
    MonoMethod* gs = nullptr;
    mono.ForEachMethod(gi, [&](MonoMethod* m) {
        if (gs) return;
        const char* n = mono.MethodName(m);
        if (!n || strcmp(n, "GetSpawnable") != 0) return;
        if (mono.ParamCount(m) != 1) return;
        if (mono.ParamTypeCode(m, 0) != 14) return; // System.String olmali
        gs = m;
    });
    if (!gs) { State().Log("[!] GetSpawnable(string) yok."); return nullptr; }
    // Tani: kayit defteri dolu mu?
    {
        void* dict = mono.GetStaticObject(gi, "_nameToSpawnable");
        int cnt = -1;
        if (dict) {
            MonoClass* dc = mono.ObjectClass(dict);
            MonoMethod* gc = dc ? mono.FindMethod(dc, "get_Count", 0) : nullptr;
            if (gc) {
                void* b = mono.Invoke(gc, dict, nullptr);
                int* pi = (int*)mono.UnboxPtr(b);
                if (pi) cnt = *pi;
            }
        }
        char cb[128];
        sprintf_s(cb, ">> _nameToSpawnable.Count = %d", cnt);
        State().Log(cb);
    }
    std::string key = Normalized(requested);
    void* ks = mono.NewString(key.c_str());
    if (!ks) { State().Log("[!] GetSpawnable: string kurulamadi."); return nullptr; }
    char kb[128];
    sprintf_s(kb, ">> GetSpawnable key: '%s'", key.c_str());
    State().Log(kb);
    void* args[1] = { ks };
    void* item = mono.Invoke(gs, nullptr, args);
    if (!item) { State().Log(">> GetSpawnable: sonuc null (dict bos ya da key yok)."); DumpAllSpawnables(gi); return nullptr; }
    // Item mi diye dogrula (sinif adi Item ile bitmeli / Creature baglantisi olmali)
    MonoClass* ic = mono.ObjectClass(item);
    if (!ic) return nullptr;
    const char* cn = mono.ClassName(ic);
    if (!cn) return nullptr;
    std::string cs = cn;
    if (cs != "Item" && cs.find("Item") == std::string::npos) {
        // Item alt sinifi da olabilir; Creature baglantisi varsa kabul et
        MonoMethod* gc = mono.FindMethod(ic, "get_Creature", 0);
        if (!gc) return nullptr;
    }
    return item;
}

// _allCreatures uzerinden prefab bul (canli ornekler arasindan isme gore).
// Creature, Item'dan turedigi icin bulunan obje dogrudan prefab olur.
static void* FindCreaturePrefab(const std::string& requested, std::string& outName) {
    auto& mono = MonoAPI::Get();
    MonoClass* gi = mono.FindClass("", "GameInfo");
    if (!gi) return nullptr;
    void* list = mono.GetStaticObject(gi, "_allCreatures");
    if (!list) { State().Log("[!] _allCreatures null."); return nullptr; }
    MonoClass* uo = mono.FindClass("UnityEngine", "Object");
    MonoMethod* nameM = uo ? mono.FindMethod(uo, "get_name", 0) : nullptr;
    if (!nameM) return nullptr;
    int n = ListCount(list);
    if (n <= 0 || n > 2000) {
        char b[128];
        sprintf_s(b, "[!] _allCreatures sayisi garip: %d", n);
        State().Log(b);
        return nullptr;
    }
    std::string want = Normalized(requested);
    void* best = nullptr;
    std::string bestName;
    for (int i = 0; i < n && i < 200; ++i) {
        void* c = ListItem(list, i);
        if (!c) continue;
        std::string nm = mono.StringUtf8(mono.Invoke(nameM, c, nullptr));
        if (nm.empty()) continue;
        std::string norm = Normalized(nm);
        State().Log(std::string("CREATURE=[") + norm + "]");
        if (norm.find(want) == std::string::npos && want.find(norm) == std::string::npos) continue;
        if (!best) { best = c; bestName = nm; }
        if (norm == want) break; // tam eslesme
    }
    if (best) {
        outName = bestName;
        State().Log(std::string("MATCH -> ") + bestName);
    }
    return best;
}

void Tick() {
    Request req;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        if (!g_req.pending) return;
        req = g_req;
        g_req.pending = false;
    }
    auto& mono = MonoAPI::Get();
    if (!ServerReady()) {
        State().Log("[!] Fish Spawner: server/host hazir degil.");
        return;
    }
    void* prefab = nullptr;
    std::string prefabName = req.name;
    // Birincil: oyunun kendi kaydi. Yedek 2: canli creature registry. Yedek 3: bait tablosu.
    prefab = GetSpawnablePrefab(req.name);
    if (prefab) {
        State().Log(std::string("[>] GameInfo kaydi: ") + req.name);
    } else {
        std::string cn;
        void* cr = FindCreaturePrefab(req.name, cn);
        if (cr) {
            prefab = cr;
            prefabName = cn;
            State().Log(std::string("[>] Canli registry: ") + cn);
        } else if (!ResolvePrefabName(req.name, prefabName, &prefab) || !prefab) {
            State().Log(std::string("[!] Fish Spawner: prefab bulunamadi: ") + req.name);
            return;
        } else {
            State().Log(std::string("[>] Fishable prefab: ") + req.name + " -> " + prefabName);
        }
    }
    Vec3 ppos{ 0,0,0 }, pfwd{ 0,0,1 };
    if (!PlayerPose(ppos, pfwd)) {
        State().Log("[!] Fish Spawner: oyuncu konumu alinamadi.");
        return;
    }
    State().Log(std::string("[+] STEP 1 OK: prefab ") + prefabName);
    MonoMethod* instM = FindInstantiate();
    if (!instM) { State().Log("[!] Fish Spawner: Instantiate bulunamadi."); return; }
    MonoClass* comp = mono.FindClass("UnityEngine", "Component");
    MonoMethod* goM = comp ? mono.FindMethod(comp, "get_gameObject", 0) : nullptr;
    if (!goM) { State().Log("[!] Fish Spawner: get_gameObject yok."); return; }
    MonoClass* cmgrC = mono.FindClass("", "CreatureManager");
    void* cmgr = cmgrC ? mono.GetStaticObject(cmgrC, "Instance") : nullptr;
    MonoMethod* aliveM = nullptr;
    if (cmgr) {
        MonoClass* mc = mono.ObjectClass(cmgr);
        if (mc) aliveM = mono.FindMethod(mc, "AddAliveCreature", 1);
    }
    // Prefab zaten Creature mi? (registry yolu) Oyleyse klon direkt Creature'dur,
    // item.Creature aramaya gerek yok.
    MonoClass* creatureC = mono.FindClass("", "Creature");
    bool prefabIsCreature = false;
    if (creatureC) {
        MonoClass* pc = mono.ObjectClass(prefab);
        for (int d = 0; pc && d < 8; ++d) {
            if (pc == creatureC) { prefabIsCreature = true; break; }
            pc = mono.GetParent(pc);
        }
    }
    if (prefabIsCreature) State().Log("[>] Prefab direkt Creature, ara adim yok.");
    int spawned = 0;
    for (int i = 0; i < req.count; ++i) {
        bool first = (i == 0);
        // Toplu spawn tek cagrida: ileri dogrultuda aralik + rastgele dagilim (ust uste binmesin)
        float fwd = 3.f + i * 1.5f;
        Vec3 at{ ppos.x + pfwd.x * fwd + RandRange(-0.4f, 0.4f), ppos.y + 0.5f,
                 ppos.z + pfwd.z * fwd + RandRange(-0.4f, 0.4f) };
        Quat id{ 0, 0, 0, 1 };
        void* ia[3] = { prefab, &at, &id };
        void* clone = mono.Invoke(instM, nullptr, ia);
        if (!clone) { if (first) State().Log("[!] STEP 2 FAIL: Instantiate"); continue; }
        if (first) State().Log("[+] STEP 2 OK: Creature Instantiate");
        // Creature bileseni: prefab zaten Creature ise klon direkt odur
        void* cr = prefabIsCreature ? clone : nullptr;
        if (!cr) {
            MonoClass* ic = mono.ObjectClass(clone);
            MonoMethod* gc = ic ? mono.FindMethodDeep(ic, "get_Creature", 0) : nullptr;
            if (gc) cr = mono.Invoke(gc, clone, nullptr);
        }
        if (!cr) { if (first) State().Log("[!] STEP 2.5 FAIL: Creature bileseni yok"); continue; }
        MonoClass* cc = mono.ObjectClass(cr);
        if (req.dead) {
            MonoMethod* kill = cc ? mono.FindMethodDeep(cc, "ServerKillOnSpawn", 0) : nullptr;
            if (kill) {
                mono.Invoke(kill, cr, nullptr);
                if (first) State().Log("[+] STEP 3 OK: ServerKillOnSpawn");
            } else if (first) State().Log("[!] STEP 3 FAIL: ServerKillOnSpawn yok (base'de de)");
        }
        void* go = mono.Invoke(goM, clone, nullptr);
        if (!go) { if (first) State().Log("[!] STEP 4 FAIL: GameObject yok"); continue; }
        if (!ServerSpawn(go)) { if (first) State().Log("[!] STEP 4 FAIL: Network Spawn (host musun?)"); continue; }
        if (first) State().Log("[+] STEP 4 OK: Network Spawn");
        if (req.drip && cc) {
            MonoMethod* drip = mono.FindMethodDeep(cc, "SetDrip", 0);
            if (drip) {
                mono.Invoke(drip, cr, nullptr);
                if (first) State().Log("[+] STEP 5 OK: SetDrip / SHINY");
            } else if (first) State().Log("[!] STEP 5 FAIL: SetDrip yok (base'de de)");
        }
        if (aliveM && cmgr) {
            void* aa[1] = { cr };
            mono.Invoke(aliveM, cmgr, aa);
        }
        ++spawned;
    }
    if (spawned == 0) {
        State().Log(std::string("[!] Fish Spawner: spawn basarisiz: ") + prefabName);
        return;
    }
    State().Log(std::string("[+] Spawn OK: ") + std::to_string(spawned) + "x " + prefabName +
        (req.dead ? " dead" : " alive") + (req.drip ? " drip" : ""));
}
} // namespace fishspawn
