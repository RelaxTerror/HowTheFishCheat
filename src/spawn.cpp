// Balik spawn gerceklemesi. Bkz. spawn.h
// Tum Unity/FishNet cagrilari main thread'de (Update hook detour) yapilir.
#include "spawn.h"
#include "cheats.h"
#include "mono_api.h"
#include "memory.h"
#include <windows.h>
#include <cstring>
#include <cmath>

struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };

namespace spawn {
static volatile LONG pending = 0;

static MonoClass *cFishable = nullptr, *cItem = nullptr, *cCreature = nullptr,
                 *cUObject = nullptr, *cResources = nullptr, *cTransform = nullptr,
                 *cComponent = nullptr;
static MonoMethod *mFindAll = nullptr; // Resources.FindObjectsOfTypeAll(Type)

static bool UnVec(void* boxed, Vec3& o) {
    void* p = MonoAPI::Get().UnboxPtr(boxed);
    if (!p) return false;
    __try { o = *(Vec3*)p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool R32(void* o, uint32_t off, uint32_t& v) {
    __try { v = *(uint32_t*)((uint8_t*)o + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool RP(void* o, uint32_t off, void*& p) {
    __try { p = *(void**)((uint8_t*)o + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool Ensure() {
    auto& mono = MonoAPI::Get();
    if (!cFishable) cFishable = mono.FindClass("", "Fishable");
    if (!cItem) cItem = mono.FindClass("", "Item");
    if (!cCreature) cCreature = mono.FindClass("", "Creature");
    if (!cUObject) cUObject = mono.FindClass("UnityEngine", "Object");
    if (!cResources) cResources = mono.FindClass("UnityEngine", "Resources");
    if (!cTransform) cTransform = mono.FindClass("UnityEngine", "Transform");
    if (!cComponent) cComponent = mono.FindClass("UnityEngine", "Component");
    if (!mFindAll && cResources) {
        // FindObjectsOfTypeAll(Type): tek parametreli overload
        mono.ForEachMethod(cResources, [&](MonoMethod* m) {
            if (mFindAll) return;
            const char* n = mono.MethodName(m);
            if (!n || strcmp(n, "FindObjectsOfTypeAll") != 0) return;
            if (mono.ParamCount(m) != 1) return;
            mFindAll = m;
        });
    }
    return cFishable && cItem && cCreature && cUObject && cResources && cTransform && cComponent && mFindAll;
}

static MonoMethod* Meth0(MonoClass* k, const char* n) {
    auto& mono = MonoAPI::Get();
    MonoMethod* m = mono.FindMethod(k, n, 0);
    if (!m) m = mono.FindMethodAny(k, n);
    return m;
}

static void* TypeOf(MonoClass* k) {
    auto& mono = MonoAPI::Get();
    MonoType* t = mono.ClassGetType(k);
    if (!t) return nullptr;
    return mono.TypeGetObject(t);
}

static int ArrLen(void* arr) {
    auto& mono = MonoAPI::Get();
    MonoClass* ac = mono.ObjectClass(arr);
    if (!ac) return -1;
    MonoMethod* gl = mono.FindMethod(ac, "get_Length", 0);
    if (!gl) return -1;
    int* pi = (int*)mono.UnboxPtr(mono.Invoke(gl, arr, nullptr));
    if (!pi) return -1;
    return *pi;
}

static void* ArrGet(void* arr, int i) {
    auto& mono = MonoAPI::Get();
    MonoClass* ac = mono.ObjectClass(arr);
    if (!ac) return nullptr;
    MonoMethod* gv = mono.FindMethod(ac, "GetValue", 1);
    if (!gv) return nullptr;
    void* ga[1] = { &i };
    return mono.Invoke(gv, arr, ga);
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

// Hedef balik turunun prefab Item'ini bul (Fishable asset taramasi)
static void* FindPrefab(const char* fishName) {
    auto& mono = MonoAPI::Get();
    void* t = TypeOf(cFishable);
    if (!t) return nullptr;
    void* args[1] = { t };
    void* arr = mono.Invoke(mFindAll, nullptr, args);
    if (!arr) return nullptr;
    int n = ArrLen(arr);
    if (n <= 0) return nullptr;
    for (int i = 0; i < n; ++i) {
        void* f = ArrGet(arr, i);
        if (!f) continue;
        MonoClass* fc = mono.ObjectClass(f);
        if (!fc) continue;
        MonoMethod* gi = mono.FindMethod(fc, "get_ItemToSpawn", 0);
        if (!gi) continue;
        void* item = mono.Invoke(gi, f, nullptr);
        if (!item) continue;
        MonoClass* ic = mono.ObjectClass(item);
        if (!ic) continue;
        MonoMethod* gc = mono.FindMethod(ic, "get_Creature", 0);
        if (!gc) continue;
        void* cr = mono.Invoke(gc, item, nullptr);
        if (!cr) continue;
        const char* cn = mono.ClassName(mono.ObjectClass(cr));
        if (cn && strcmp(cn, fishName) == 0) return item;
    }
    return nullptr;
}

static bool PlayerPose(Vec3& pos, Vec3& fwd) {
    auto& mono = MonoAPI::Get();
    void* lp = LocalPlayer();
    if (!lp) return false;
    MonoClass* pc = mono.ObjectClass(lp);
    if (!pc) return false;
    void* tr = nullptr;
    {
        uint32_t off = 0;
        if (mono.GetFieldOffset(pc, "_transform", off)) RP(lp, off, tr);
    }
    if (!tr) {
        MonoMethod* gt = Meth0(pc, "get_Transform");
        if (!gt) return false;
        tr = mono.Invoke(gt, lp, nullptr);
        if (!tr) return false;
    }
    MonoMethod* gp = Meth0(cTransform, "get_position");
    MonoMethod* gf = Meth0(cTransform, "get_forward");
    if (!gp || !gf) return false;
    return UnVec(mono.Invoke(gp, tr, nullptr), pos) && UnVec(mono.Invoke(gf, tr, nullptr), fwd);
}

// ServerManager.Spawn(GameObject, null, default) — FishNet resmi server spawn
static bool ServerSpawn(void* go) {
    auto& mono = MonoAPI::Get();
    MonoClass* ifc = mono.FindClass("FishNet", "InstanceFinder");
    if (!ifc) ifc = mono.FindClass("", "InstanceFinder");
    if (!ifc) return false;
    MonoMethod* gsm = mono.FindMethod(ifc, "get_ServerManager", 0);
    if (!gsm) return false;
    void* sm = mono.Invoke(gsm, nullptr, nullptr);
    if (!sm) return false;
    MonoClass* sc = mono.ObjectClass(sm);
    if (!sc) return false;
    // Spawn(GameObject, NetworkConnection, Scene) — 3 parametreli
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
    void* exc = nullptr;
    (void)exc;
    mono.Invoke(sp, sm, args);
    return true; // InvokeChecked yok: exception olursa oyun loglar, devam eder
}

static void DoSpawn() {
    auto& st = State();
    auto& mono = MonoAPI::Get();
    static const char* fish[] = { "Cod", "Triggerfish" };
    int idx = st.spawnIdx % 2;
    if (idx < 0) idx = 0;
    const char* want = fish[idx];
    if (!Ensure()) { s.Log("[!] Spawn: siniflar bulunamadi."); return; }
    void* prefab = FindPrefab(want);
    if (!prefab) {
        char b[128];
        sprintf_s(b, "[!] %s prefabi bulunamadi.", want);
        s.Log(b);
        return;
    }
    Vec3 ppos, pfwd;
    if (!PlayerPose(ppos, pfwd)) { s.Log("[!] Oyuncu konumu alinamadi."); return; }
    Vec3 at{ ppos.x + pfwd.x * 3.f, ppos.y + 0.5f, ppos.z + pfwd.z * 3.f };
    Quat id{ 0, 0, 0, 1 };
    // Object.Instantiate(Object, Vector3, Quaternion) non-generic
    MonoMethod* inst = nullptr;
    mono.ForEachMethod(cUObject, [&](MonoMethod* m) {
        if (inst) return;
        const char* n = mono.MethodName(m);
        if (!n || strcmp(n, "Instantiate") != 0) return;
        if (mono.ParamCount(m) != 3) return;
        if (mono.ParamTypeCode(m, 1) != 17) return; // Vector3
        if (mono.ParamTypeCode(m, 2) != 17) return; // Quaternion
        inst = m;
    });
    if (!inst) { s.Log("[!] Instantiate bulunamadi."); return; }
    void* ia[3] = { prefab, &at, &id };
    void* clone = mono.Invoke(inst, nullptr, ia);
    if (!clone) { s.Log("[!] Instantiate basarisiz."); return; }
    // Creature bileseni: once Item.get_Creature, yedek GetComponent(Creature)
    void* cr = nullptr;
    {
        MonoClass* ic = mono.ObjectClass(clone);
        MonoMethod* gc = ic ? mono.FindMethod(ic, "get_Creature", 0) : nullptr;
        if (gc) cr = mono.Invoke(gc, clone, nullptr);
    }
    if (!cr) {
        void* t = TypeOf(cCreature);
        if (t) {
            MonoMethod* gcomp = nullptr;
            mono.ForEachMethod(cComponent, [&](MonoMethod* m) {
                if (gcomp) return;
                const char* n = mono.MethodName(m);
                if (!n || strcmp(n, "GetComponent") != 0) return;
                if (mono.ParamCount(m) != 1) return;
                if (mono.ParamTypeCode(m, 0) != 18) return; // Type (string degil)
                gcomp = m;
            });
            if (gcomp) {
                void* ga[1] = { t };
                cr = mono.Invoke(gcomp, clone, ga);
            }
        }
    }
    if (!cr) { s.Log("[!] Creature bileseni yok."); return; }
    if (st.spawnDead) {
        MonoClass* cc = mono.ObjectClass(cr);
        MonoMethod* kill = cc ? mono.FindMethod(cc, "ServerKillOnSpawn", 0) : nullptr;
        if (kill) mono.Invoke(kill, cr, nullptr);
    }
    // GameObject + server spawn
    MonoMethod* gg = Meth0(cComponent, "get_gameObject");
    void* go = gg ? mono.Invoke(gg, clone, nullptr) : nullptr;
    if (!go) { s.Log("[!] GameObject alinamadi."); return; }
    if (!ServerSpawn(go)) { s.Log("[!] Server spawn basarisiz (host degil misin?)."); return; }
    if (st.spawnShiny) {
        MonoClass* cc = mono.ObjectClass(cr);
        MonoMethod* drip = cc ? mono.FindMethod(cc, "SetDrip", 0) : nullptr;
        if (drip) mono.Invoke(drip, cr, nullptr);
    }
    // AI kaydi
    {
        MonoClass* cmgr = mono.FindClass("", "CreatureManager");
        void* mgr = cmgr ? mono.GetStaticObject(cmgr, "Instance") : nullptr;
        if (mgr) {
            MonoClass* mc = mono.ObjectClass(mgr);
            MonoMethod* add = mc ? mono.FindMethod(mc, "AddAliveCreature", 1) : nullptr;
            if (add) { void* aa[1] = { cr }; mono.Invoke(add, mgr, aa); }
        }
    }
    char b[160];
    sprintf_s(b, "[+] %s spawnlandi%s%s.", want, st.spawnShiny ? " (shiny)" : "", st.spawnDead ? " (olu)" : "");
    s.Log(b);
}

void Request() {
    InterlockedExchange(&pending, 1);
    State().Log("[*] Spawn kuyrukta (1 frame icinde).");
}

void Tick() {
    if (!InterlockedCompareExchange(&pending, 0, 0)) return;
    InterlockedExchange(&pending, 0);
    __try { DoSpawn(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { State().Log("[!] Spawn sirasinda hata."); }
}
} // namespace spawn
