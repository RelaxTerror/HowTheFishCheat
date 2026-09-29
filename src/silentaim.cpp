// Silent aim gerceklemesi. Bkz. silentaim.h
// Tum Unity cagrilari Shoot detour'undan (oyunun main thread'i) yapilir,
// o yuzden thread-guvenlidir. Her adimda hata = normal atis (sessiz basarisizlik).
#include "silentaim.h"
#include "cheats.h"
#include "mono_api.h"
#include "memory.h"
#include "fishspawn.h"
#include "MinHook.h"
#include <windows.h>
#include <cstring>
#include <cstdio>
#include <cmath>

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };

namespace silentaim {
typedef void (__cdecl* ShootFn)(void*);
static ShootFn oShoot = nullptr;
typedef void (__cdecl* AddModelRecoilFn)(void*);
static AddModelRecoilFn oAddModelRecoil = nullptr;
typedef void (__cdecl* RecoilVec2Fn)(void*, Vec2);
static RecoilVec2Fn oCameraRecoil = nullptr;
static RecoilVec2Fn oToolRecoil = nullptr;
typedef void (__cdecl* CameraMouseFn)(void*);
static CameraMouseFn oCameraMouseMovement = nullptr;
typedef void (__cdecl* PlayerUpdateFn)(void*);
static PlayerUpdateFn oPlayerUpdate = nullptr;
static bool hooked = false;
static bool frameHooked = false;
static volatile LONG inShoot = 0;
static volatile LONG inPreview = 0;
static ULONGLONG lastPreviewMs = 0;
static ULONGLONG lastFullScanMs = 0;

// Sinif onbellekleri (process omru boyunca gecerli)
static MonoClass *cPlayer = nullptr, *cFish = nullptr, *cBird = nullptr,
                 *cAlbatross = nullptr,
                 *cCreature = nullptr, *cUObject = nullptr, *cTransform = nullptr,
                 *cComponent = nullptr, *cCollider = nullptr, *cArray = nullptr;
static MonoMethod *mFindByType = nullptr, *mGetCompInChild = nullptr,
                  *mGetLength = nullptr, *mGetValue = nullptr,
                  *mGetTransform = nullptr, *mRaycast = nullptr;
static MonoClass *cPhysics = nullptr;

// Ham okuma/yazma (POD only -> __try serbest)
static bool R32(void* o, uint32_t off, uint32_t& v) {
    __try { v = *(uint32_t*)((uint8_t*)o + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool RW32(void* o, uint32_t off, uint32_t v) {
    __try { *(uint32_t*)((uint8_t*)o + off) = v; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool RP(void* o, uint32_t off, void*& p) {
    __try { p = *(void**)((uint8_t*)o + off); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static float R2F(uint32_t r) { float f; memcpy(&f, &r, 4); return f; }

static bool EnsureClasses() {
    auto& mono = MonoAPI::Get();
    if (!cPlayer) cPlayer = mono.FindClass("", "Player");
    if (!cFish) cFish = mono.FindClass("", "Fish");
    if (!cBird) cBird = mono.FindClass("", "Bird");
    if (!cAlbatross) cAlbatross = mono.FindClass("", "Albatross"); // yoksa nullptr kalir, fallback kapsar
    if (!cCreature) cCreature = mono.FindClass("", "Creature");
    if (!cUObject) cUObject = mono.FindClass("UnityEngine", "Object");
    if (!cTransform) cTransform = mono.FindClass("UnityEngine", "Transform");
    if (!cComponent) cComponent = mono.FindClass("UnityEngine", "Component");
    if (!cCollider) cCollider = mono.FindClass("UnityEngine", "Collider");
    if (!cArray) cArray = mono.FindClass("System", "Array");
    return cPlayer && cFish && cBird && cCreature && cUObject && cTransform && cComponent && cCollider && cArray;
}

// Type alan tek-parametreli, generic olmayan overload'u sec. Unity'de ayni isimde
// FindObjectsOfType<T>(bool) / GetComponentsInChildren<T>(bool) da bulunuyor;
// yalnizca parametre sayisina bakmak bazen bool overload'unu secip taramayi bozuyordu.
static MonoMethod* FindTypeMethod1(MonoClass* klass, const char* name) {
    auto& mono = MonoAPI::Get();
    MonoMethod* found = nullptr;
    mono.ForEachMethod(klass, [&](MonoMethod* m) {
        if (found) return;
        const char* n = mono.MethodName(m);
        if (!n || strcmp(n, name) != 0) return;
        if (mono.ParamCount(m) != 1) return;
        const char* pn = mono.ParamClassName(m, 0);
        if (!pn || strcmp(pn, "Type") != 0) return;
        found = m;
    });
    return found;
}

static MonoMethod* FindIntMethod1(MonoClass* klass, const char* name) {
    auto& mono = MonoAPI::Get();
    MonoMethod* found = nullptr;
    mono.ForEachMethod(klass, [&](MonoMethod* m) {
        if (found) return;
        const char* n = mono.MethodName(m);
        if (!n || strcmp(n, name) != 0 || mono.ParamCount(m) != 1) return;
        if (mono.ParamTypeCode(m, 0) == 8) found = m; // System.Int32
    });
    return found;
}

static MonoMethod* FindMethodDeep(MonoClass* klass, const char* name, int args) {
    auto& mono = MonoAPI::Get();
    for (MonoClass* c = klass; c; c = mono.GetParent(c)) {
        MonoMethod* m = mono.FindMethod(c, name, args);
        if (m) return m;
    }
    return nullptr;
}

static bool EnsureMethods() {
    auto& mono = MonoAPI::Get();
    if (!mFindByType) mFindByType = FindTypeMethod1(cUObject, "FindObjectsOfType");
    if (!mGetCompInChild) mGetCompInChild = FindTypeMethod1(cComponent, "GetComponentsInChildren");
    if (!mGetLength) mGetLength = mono.FindMethod(cArray, "get_Length", 0);
    if (!mGetValue) mGetValue = FindIntMethod1(cArray, "GetValue");
    if (!mGetTransform) mGetTransform = mono.FindMethod(cComponent, "get_transform", 0);
    if (!mRaycast) {
        if (!cPhysics) cPhysics = mono.FindClass("UnityEngine", "Physics");
        // Raycast(Vector3 origin, Vector3 yon, float mesafe) -> bool.
        // Imza tam sabitlenir (17,17,12): (Ray,float,int) gibi komsu
        // overload'lar elenir, yanlis imza rastgele LoS demekti.
        if (cPhysics) {
            mono.ForEachMethod(cPhysics, [&](MonoMethod* m) {
                if (mRaycast) return;
                const char* n = mono.MethodName(m);
                if (!n || strcmp(n, "Raycast") != 0) return;
                if (mono.ParamCount(m) != 3) return;
                if (mono.ParamTypeCode(m, 0) != 17) return;
                if (mono.ParamTypeCode(m, 1) != 17) return;
                if (mono.ParamTypeCode(m, 2) != 12) return;
                mRaycast = m;
            });
        }
    }
    return mFindByType && mGetCompInChild && mGetLength && mGetValue && mGetTransform;
}

static MonoMethod* Meth0(MonoClass* k, const char* n) {
    auto& mono = MonoAPI::Get();
    MonoMethod* m = mono.FindMethod(k, n, 0);
    if (!m) m = mono.FindMethodAny(k, n);
    return m;
}

static bool UnVec(void* boxed, Vec3& o) {
    void* p = MonoAPI::Get().UnboxPtr(boxed);
    if (!p) return false;
    __try { o = *(Vec3*)p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool UnQuat(void* boxed, Quat& o) {
    void* p = MonoAPI::Get().UnboxPtr(boxed);
    if (!p) return false;
    __try { o = *(Quat*)p; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool UnBool(void* boxed, bool& o) {
    void* p = MonoAPI::Get().UnboxPtr(boxed);
    if (!p) return false;
    __try { o = (*(uint8_t*)p != 0); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void* TypeOf(MonoClass* k) {
    auto& mono = MonoAPI::Get();
    MonoType* t = mono.ClassGetType(k);
    if (!t) return nullptr;
    return mono.TypeGetObject(t);
}

static void* LocalPlayer() {
    auto& mono = MonoAPI::Get();
    const char* names[] = { "LocalPlayer", "<LocalPlayer>k__BackingField",
        "_localPlayer", "localPlayer", "Instance", "_instance", "Local" };
    for (auto* n : names) {
        void* o = mono.GetStaticObject(cPlayer, n);
        if (o && mono.IsInstanceOf(o, cPlayer)) return o;
    }
    return nullptr;
}

// Iki Player ayni kisi mi? (host'ta server+client kopyalari farkli objedir)
static bool SamePlayer(void* a, void* b) {
    if (!a || !b) return false;
    if (a == b) return true;
    auto& mono = MonoAPI::Get();
    uint32_t o1 = 0, o2 = 0;
    MonoClass *ca = mono.ObjectClass(a), *cb = mono.ObjectClass(b);
    if (!ca || !cb) return false;
    if (!mono.GetFieldOffset(ca, "_steamID", o1) || !mono.GetFieldOffset(cb, "_steamID", o2)) return false;
    void *sa = nullptr, *sb = nullptr;
    if (!RP(a, o1, sa) || !RP(b, o2, sb) || !sa || !sb) return false;
    MonoClass *sca = mono.ObjectClass(sa), *scb = mono.ObjectClass(sb);
    if (!sca || !scb) return false;
    uint32_t v1 = 0, v2 = 0;
    if (!mono.GetFieldOffset(sca, "_value", v1) || !mono.GetFieldOffset(scb, "_value", v2)) return false;
    uint64_t x = 0, y = 0;
    __try {
        x = *(uint64_t*)((uint8_t*)sa + v1);
        y = *(uint64_t*)((uint8_t*)sb + v2);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return x != 0 && x == y;
}

static bool IsSubclassOf(MonoClass* child, MonoClass* base) {
    if (!child || !base || child == base) return false;
    auto& mono = MonoAPI::Get();
    for (MonoClass* c = mono.GetParent(child); c; c = mono.GetParent(c))
        if (c == base) return true;
    return false;
}

static bool IsHuntTarget(void* o) {
    auto& mono = MonoAPI::Get();
    MonoClass* c = mono.ObjectClass(o);
    for (int i = 0; c && i < 10; ++i) {
        if (c == cFish || c == cBird) return true;
        if (cAlbatross && c == cAlbatross) return true;
        c = mono.GetParent(c);
    }
    // Albatross sinifi cozulemezse (isim/namespace degismis olabilir) hedefi kacirma:
    // Creature soyundan gelen bilinmeyen alt sinifi da kabul et.
    if (!cAlbatross) {
        c = mono.ObjectClass(o);
        for (int i = 0; c && i < 10; ++i) {
            if (c == cCreature) return true;
            c = mono.GetParent(c);
        }
    }
    return false;
}

static int TargetKind(void* o) {
    auto& mono = MonoAPI::Get();
    MonoClass* c = mono.ObjectClass(o);
    for (int i = 0; c && i < 10; ++i) {
        if (c == cFish) return 1;
        if (c == cBird) return 2;
        if (cAlbatross && c == cAlbatross) return 3;
        c = mono.GetParent(c);
    }
    return 0;
}

static float VecAng(Vec3 a, Vec3 b) {
    float la = sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
    float lb = sqrtf(b.x * b.x + b.y * b.y + b.z * b.z);
    if (la < 1e-6f || lb < 1e-6f) return 180.f;
    float d = (a.x * b.x + a.y * b.y + a.z * b.z) / (la * lb);
    if (d > 1.f) d = 1.f;
    if (d < -1.f) d = -1.f;
    return acosf(d) * 57.29578f;
}

// Kamera: LocalPlayer._camera -> get_CamTransform -> position/forward
static bool CamPose(Vec3& pos, Vec3& fwd) {
    auto& mono = MonoAPI::Get();
    void* lp = LocalPlayer();
    if (!lp) return false;
    uint32_t off = 0;
    void* camH = nullptr;
    if (mono.GetFieldOffset(cPlayer, "_camera", off))
        RP(lp, off, camH);
    if (!camH) {
        MonoMethod* gm = Meth0(cPlayer, "get_Camera");
        if (!gm) return false;
        camH = mono.Invoke(gm, lp, nullptr);
        if (!camH) return false;
    }
    MonoClass* pcc = mono.ObjectClass(camH);
    if (!pcc) return false;
    MonoMethod* gt = mono.FindMethod(pcc, "get_CamTransform", 0);
    if (!gt) return false;
    void* tr = mono.Invoke(gt, camH, nullptr);
    if (!tr) return false;
    MonoMethod* gp = Meth0(cTransform, "get_position");
    MonoMethod* gf = Meth0(cTransform, "get_forward");
    if (!gp || !gf) return false;
    return UnVec(mono.Invoke(gp, tr, nullptr), pos) && UnVec(mono.Invoke(gf, tr, nullptr), fwd);
}

static bool TargetGeometry(void* c, Vec3& out, Vec3& ext) {
    auto& mono = MonoAPI::Get();
    ext = { .45f, .45f, .45f };
    void* arr = nullptr;
    {
        void* t = TypeOf(cCollider);
        if (!t) return false;
        void* args[1] = { t };
        arr = mono.Invoke(mGetCompInChild, c, args);
    }
    if (arr) {
        int n = 0;
        void* bl = mono.Invoke(mGetLength, arr, nullptr);
        int* pi = (int*)mono.UnboxPtr(bl);
        if (pi) n = *pi;
        // Tum collider'lari dene, hacmi EN KUCUK olani al: ilk collider
        // bazen dev bir trigger/alan olur (ozellikle ucan hedeflerde) ve
        // mermi bos noktaya gider. Gercek hitbox genelde en kucugudur.
        bool got = false;
        float bestVol = 0.f;
        for (int i = 0; i < n; ++i) {
            int idx = i;
            void* ga[1] = { &idx };
            void* col = mono.Invoke(mGetValue, arr, ga);
            if (!col) continue;
            MonoClass* cc = mono.ObjectClass(col);
            MonoMethod* gb = cc ? mono.FindMethod(cc, "get_bounds", 0) : nullptr;
            if (!gb) continue;
            void* bb = mono.Invoke(gb, col, nullptr);
            void* p = mono.UnboxPtr(bb);
            if (!p) continue;
            __try {
                Vec3 ctr = ((Vec3*)p)[0];
                Vec3 ex = ((Vec3*)p)[1];
                float vol = fabsf(ex.x * ex.y * ex.z);
                if (!got || vol < bestVol) {
                    got = true; bestVol = vol;
                    out = ctr; ext = ex;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
        if (got) return true;
    }
    // yedek: transform.position
    void* tr = mono.Invoke(mGetTransform, c, nullptr);
    if (!tr) return false;
    MonoMethod* gp = Meth0(cTransform, "get_position");
    if (!gp) return false;
    return UnVec(mono.Invoke(gp, tr, nullptr), out);
}

static bool TargetCenter(void* c, Vec3& out) {
    Vec3 ext;
    return TargetGeometry(c, out, ext);
}

// Hafif konum: her frame cagrilan takip icin collider taramasi YOK,
// dogrudan transform.position (ucan hedefte de gecerli).
static bool TargetPositionFast(void* c, Vec3& out) {
    auto& mono = MonoAPI::Get();
    void* tr = mono.Invoke(mGetTransform, c, nullptr);
    if (!tr) return false;
    MonoMethod* gp = Meth0(cTransform, "get_position");
    if (!gp) return false;
    return UnVec(mono.Invoke(gp, tr, nullptr), out);
}

// Gorunurluk: origin'den hedefe giden isin hedef DISINDA bir seye carpiyor mu?
// reach hedefin kendi buyuklugune gore ayarlanir; sabit 1.5m, kanatli/iri
// hedeflerin kendi collider'ini vurup kilidi dusuruyordu.
static bool HasLineOfSight(const Vec3& origin, const Vec3& tp, const Vec3& te) {
    auto& mono = MonoAPI::Get();
    if (!mRaycast) return true;
    Vec3 to{ tp.x - origin.x, tp.y - origin.y, tp.z - origin.z };
    float dist = sqrtf(to.x * to.x + to.y * to.y + to.z * to.z);
    if (dist < 4.f) return true; // cok yakin: namlu/kamera ici temaslari yoksay
    float maxE = te.x;
    if (te.y > maxE) maxE = te.y;
    if (te.z > maxE) maxE = te.z;
    float reach = dist - (maxE + 0.5f);
    if (reach < 0.5f) return true;
    Vec3 dn{ to.x / dist, to.y / dist, to.z / dist };
    void* ra[3] = { (void*)&origin, &dn, &reach };
    void* rb = mono.Invoke(mRaycast, nullptr, ra);
    bool* hit = (bool*)mono.UnboxPtr(rb);
    return !(hit && *hit);
}

// Tek dogruluk kapisi: ESP onizleme VE ates ani AYNI testi kullanir.
// (Eskiden onizleme LoS bakmiyordu: kilit gorunup mermi gitmiyordu.)
// Not: selectedExtents'in altinda tanimli, o yuzden PublishSnapshot sonrasinda.
// Elenme nedeni: 1=olu/bilinmeyen, 2=geometri, 3=mesafe, 4=fov, 5=blok(LoS).
static bool ValidateTarget(void* c, const Vec3& cpos, const Vec3& cfwd,
                           float fovLim, float maxD, Vec3& tp, Vec3& te, bool fast,
                           int* rej = nullptr);

struct ShotPlan {
    bool redirect = false;
    void* fp = nullptr;      // FirePoint transform
    Quat savedQ{ 0,0,0,1 };
    float spread = 0.f;
    uint32_t spreadOff = 0;
    MonoClass* wcls = nullptr;
};

struct NoRecoilPlan {
    bool spreadSaved = false;
    bool knockbackSaved = false;
    uint32_t spreadOff = 0;
    uint32_t knockbackOff = 0;
    uint32_t spreadRaw = 0;
    uint32_t knockbackRaw = 0;
};

static void ApplyNoRecoilForShot(void* self, NoRecoilPlan& plan) {
    if (!self || !State().noRecoil) return;
    auto& mono = MonoAPI::Get();
    MonoClass* wc = mono.ObjectClass(self);
    if (!wc) return;
    if (mono.GetFieldOffset(wc, "_spread", plan.spreadOff) &&
        R32(self, plan.spreadOff, plan.spreadRaw)) {
        plan.spreadSaved = RW32(self, plan.spreadOff, 0);
    }
    if (mono.GetFieldOffset(wc, "_recoilKnockback", plan.knockbackOff) &&
        R32(self, plan.knockbackOff, plan.knockbackRaw)) {
        plan.knockbackSaved = RW32(self, plan.knockbackOff, 0);
    }
}

static void RestoreNoRecoilAfterShot(void* self, const NoRecoilPlan& plan) {
    if (!self) return;
    if (plan.spreadSaved) RW32(self, plan.spreadOff, plan.spreadRaw);
    if (plan.knockbackSaved) RW32(self, plan.knockbackOff, plan.knockbackRaw);
}

static void __cdecl HookAddModelRecoil(void* self) {
    if (State().noRecoil) return;
    if (oAddModelRecoil) oAddModelRecoil(self);
}

static void __cdecl HookCameraRecoil(void* self, Vec2 recoil) {
    if (State().noRecoil) {
        // Yeni recoil ekleme ve onceden biriken kamera recoil state'ini de temizle.
        auto& mono = MonoAPI::Get();
        MonoClass* c = mono.ObjectClass(self);
        uint32_t off = 0;
        if (c && mono.GetFieldOffset(c, "_recoilTar", off)) {
            RW32(self, off, 0); RW32(self, off + 4, 0);
        }
        if (c && mono.GetFieldOffset(c, "_recoilCur", off)) {
            RW32(self, off, 0); RW32(self, off + 4, 0);
        }
        return;
    }
    if (oCameraRecoil) oCameraRecoil(self, recoil);
}

static void __cdecl HookToolRecoil(void* self, Vec2 recoil) {
    if (State().noRecoil) return;
    if (oToolRecoil) oToolRecoil(self, recoil);
}

static void ClearCameraRecoilState(void* self) {
    if (!self) return;
    auto& mono = MonoAPI::Get();
    MonoClass* c = mono.ObjectClass(self);
    if (!c) return;
    uint32_t off = 0;
    if (mono.GetFieldOffset(c, "_recoilTar", off)) {
        RW32(self, off, 0); RW32(self, off + 4, 0);
    }
    if (mono.GetFieldOffset(c, "_recoilCur", off)) {
        RW32(self, off, 0); RW32(self, off + 4, 0);
    }
}

static void __cdecl HookCameraMouseMovement(void* self) {
    if (State().noRecoil) ClearCameraRecoilState(self);
    if (oCameraMouseMovement) oCameraMouseMovement(self);
}

// Kamera dikey FOV (ESP projeksiyonu icin). Main thread.
static float CamFovY() {
    auto& mono = MonoAPI::Get();
    MonoClass* cam = mono.FindClass("UnityEngine", "Camera");
    if (!cam) return 60.f;
    MonoMethod* gm = mono.FindMethod(cam, "get_main", 0);
    if (!gm) return 60.f;
    void* main = mono.Invoke(gm, nullptr, nullptr);
    if (!main) return 60.f;
    MonoClass* mc = mono.ObjectClass(main);
    if (!mc) return 60.f;
    MonoMethod* gf = mono.FindMethod(mc, "get_fieldOfView", 0);
    if (!gf) return 60.f;
    void* b = mono.Invoke(gf, main, nullptr);
    float* pf = (float*)mono.UnboxPtr(b);
    if (!pf) return 60.f;
    float f = 60.f;
    __try { f = *pf; } __except (EXCEPTION_EXECUTE_HANDLER) { return 60.f; }
    if (f < 10.f || f > 120.f) return 60.f;
    return f;
}

// Tek hedef secici: hem canli ESP hem de atis ayni sonucu kullanir.
// Unity taramasi yalnizca oyun Update/Shoot main thread'inde yapilir.
static void* selectedTarget = nullptr;
static Vec3 selectedCenter{ 0,0,0 };
static Vec3 selectedExtents{ .45f,.45f,.45f };
static float selectedDistSq = 0.f;
// Hedef hiz takibi (lead icin): ayni hedef ust uste goruldukce yumusatilmis hiz.
static void* trackTarget = nullptr;
static Vec3 trackPrev{ 0,0,0 };
static Vec3 trackVel{ 0,0,0 };
static ULONGLONG trackMs = 0;
static bool trackValid = false;

static void PublishSnapshot(void* target, const Vec3& cpos, const Vec3& cfwd, const Vec3& center,
                            const Vec3& extents, float distSq, int kind) {
    auto& st = State();
    // Hiz olcumu: hedef degismisse sifirla, yoksa (merkez farki / sure) + yumusatma.
    ULONGLONG now = GetTickCount64();
    if (target && target == trackTarget && trackMs != 0 && now > trackMs) {
        ULONGLONG dt = now - trackMs;
        if (dt > 0 && dt < 500) {
            float s = (float)dt / 1000.f;
            Vec3 inst{ (center.x - trackPrev.x) / s, (center.y - trackPrev.y) / s, (center.z - trackPrev.z) / s };
            float spd = sqrtf(inst.x * inst.x + inst.y * inst.y + inst.z * inst.z);
            if (spd < 60.f) { // isinlanma/yukleme sicramasini ele
                trackVel.x += (inst.x - trackVel.x) * 0.5f;
                trackVel.y += (inst.y - trackVel.y) * 0.5f;
                trackVel.z += (inst.z - trackVel.z) * 0.5f;
                trackValid = true;
            } else trackValid = false;
        } else trackValid = false;
    } else if (target != trackTarget) {
        trackTarget = target;
        trackVel = { 0,0,0 };
        trackValid = false;
    }
    trackPrev = center;
    trackMs = now;
    selectedCenter = center;
    selectedExtents = extents;
    selectedDistSq = distSq;
    st.espTx = center.x; st.espTy = center.y; st.espTz = center.z;
    st.espEx = extents.x; st.espEy = extents.y; st.espEz = extents.z;
    st.espTargetKind = kind;
    st.espCx = cpos.x; st.espCy = cpos.y; st.espCz = cpos.z;
    st.espFx = cfwd.x; st.espFy = cfwd.y; st.espFz = cfwd.z;
    st.espDist = sqrtf(distSq);
    st.espFovY = CamFovY();
    st.espValid = true;
}

// ValidateTarget govdesi (selectedExtents burada biliniyor).
static bool ValidateTarget(void* c, const Vec3& cpos, const Vec3& cfwd,
                           float fovLim, float maxD, Vec3& tp, Vec3& te, bool fast,
                           int* rej) {
    auto& mono = MonoAPI::Get();
    MonoClass* cc = mono.ObjectClass(c);
    MonoMethod* gd = cc ? FindMethodDeep(cc, "get_IsDead", 0) : nullptr;
    if (!gd) {
        // Boss Albatros'ta IsDead yoksa olu sayma: canli kabul et (boss bar zaten gosteriyor).
        if (TargetKind(c) != 3) { if (rej) *rej = 1; return false; }
    } else {
        bool dead = false;
        if (!UnBool(mono.Invoke(gd, c, nullptr), dead) || dead) { if (rej) *rej = 1; return false; }
    }
    if (fast) {
        if (!TargetPositionFast(c, tp)) { if (rej) *rej = 2; return false; }
        te = selectedExtents; // son bilinen olcu korunur
    } else {
        if (!TargetGeometry(c, tp, te)) { if (rej) *rej = 2; return false; }
    }
    Vec3 to{ tp.x - cpos.x, tp.y - cpos.y, tp.z - cpos.z };
    float ds = to.x * to.x + to.y * to.y + to.z * to.z;
    if (ds < 1e-6f || ds > maxD * maxD) { if (rej) *rej = 3; return false; }
    if (VecAng(cfwd, to) > fovLim) { if (rej) *rej = 4; return false; }
    if (!HasLineOfSight(cpos, tp, te)) { if (rej) *rej = 5; return false; }
    return true;
}

static bool SelectAndPublish(const Vec3& cpos, const Vec3& cfwd) {
    auto& st = State();
    auto& mono = MonoAPI::Get();
    st.espValid = false;
    selectedTarget = nullptr;
    if (!st.silentAim || !EnsureClasses() || !EnsureMethods()) return false;

    void* ctype = TypeOf(cCreature);
    if (!ctype) return false;
    void* aa[1] = { ctype };
    void* arr = mono.Invoke(mFindByType, nullptr, aa);
    if (!arr) return false;
    void* bl = mono.Invoke(mGetLength, arr, nullptr);
    int* pi = (int*)mono.UnboxPtr(bl);
    if (!pi) return false;

    // FOV burada YARI-acidir (ESP dairesiyle ayni: daire fov*0.5 ile cizilir).
    // Eskiden tam-aci kullaniliyordu: dairenin 2 kati disina da kilitleniyordu.
    float fov = st.silentFov;
    if (fov < 1.f) fov = 1.f;
    if (fov > 80.f) fov = 80.f;
    float fovLim = fov * 0.5f;
    float maxD = st.silentMaxDist;
    if (maxD < 5.f) maxD = 5.f;
    if (maxD > 800.f) maxD = 800.f;

    void* best = nullptr;
    Vec3 bestC{ 0,0,0 };
    Vec3 bestE{ .45f,.45f,.45f };
    float bestA = fovLim, bestD = 3.402823e38f;
    // Tek seferlik dagilim dokumu: Albatross sahnede mi, neden eleniyor?
    static bool s_diagDone = false;
    int nFish = 0, nBird = 0, nAlb = 0, nOther = 0;
    int nDead = 0, nFar = 0, nLos = 0;
    // Aday havuzu: Creature taramasi + AYRI hiyerarsideki Albatross dogrudan tarama.
    // (Boss Albatros Creature'dan turemyorsa ilk dizide HIC gorunmez!)
    bool albSeparate = cAlbatross && !IsSubclassOf(cAlbatross, cCreature);
    auto consider = [&](void* c) {
        if (!c || !IsHuntTarget(c)) {
            if (c && !s_diagDone) {
                MonoClass* cc0 = mono.ObjectClass(c);
                bool isCreature = (cc0 == cCreature) || IsSubclassOf(cc0, cCreature);
                if (isCreature) ++nOther;
            }
            return;
        }
        int k = 0;
        if (!s_diagDone) {
            k = TargetKind(c);
            if (k == 1) ++nFish; else if (k == 2) ++nBird; else if (k == 3) ++nAlb;
        }
        Vec3 tp, te;
        int rej = 0;
        if (!ValidateTarget(c, cpos, cfwd, fovLim, maxD, tp, te, false, &rej)) {
            if (!s_diagDone) {
                if (rej == 1) ++nDead; else if (rej == 3) ++nFar; else if (rej == 5) ++nLos;
            }
            return;
        }
        Vec3 to{ tp.x - cpos.x, tp.y - cpos.y, tp.z - cpos.z };
        float ds = to.x * to.x + to.y * to.y + to.z * to.z;
        float ang = VecAng(cfwd, to);
        float diff = ang - bestA;
        if (diff < -1e-4f || (fabsf(diff) <= 1e-4f && ds < bestD)) {
            bestA = ang; bestD = ds; best = c; bestC = tp; bestE = te;
        }
    };
    for (int i = 0; i < *pi; ++i) {
        void* ga[1] = { &i };
        consider(mono.Invoke(mGetValue, arr, ga));
    }
    int nAlbExtra = 0;
    if (albSeparate) {
        void* atype = TypeOf(cAlbatross);
        if (atype) {
            void* ab[1] = { atype };
            void* aarr = mono.Invoke(mFindByType, nullptr, ab);
            if (aarr) {
                void* bl2 = mono.Invoke(mGetLength, aarr, nullptr);
                int* pi2 = (int*)mono.UnboxPtr(bl2);
                if (pi2) for (int i = 0; i < *pi2; ++i) {
                    void* ga[1] = { &i };
                    void* c = mono.Invoke(mGetValue, aarr, ga);
                    if (c) ++nAlbExtra;
                    consider(c);
                }
            }
        }
    }
    if (!s_diagDone) {
        s_diagDone = true;
        char dg[256];
        sprintf_s(dg, ">> Tara: %d Creature%s (Fish x%d, Bird x%d, Albatross x%d) | elenen: uzak x%d, blok x%d, olu x%d%s",
            *pi, albSeparate ? " +AYRI-Alb" : "", nFish, nBird, nAlb, nFar, nLos, nDead,
            nOther ? " +filtre-disi Creature var!" : "");
        st.Log(dg);
        if (albSeparate)
            st.Log(nAlbExtra ? "[+] Albatross AYRI sinif, dogrudan tarandi."
                             : "[!] Albatross AYRI sinif ama sahnede bulunamadi!");
        else if (!cAlbatross)
            st.Log("[!] Albatross sinifi cozulemedi (fallback aktif).");
    }
    if (!best) return false;

    selectedTarget = best;
    PublishSnapshot(best, cpos, cfwd, bestC, bestE, bestD, TargetKind(best));
    return true;
}

// Tam liste taramadan mevcut hedefin hareketini ve kamera pozunu yeniler.
// Hafif yol her frame calisir (collider taramasi yok); tam secim ~10 Hz.
// Ayni ValidateTarget kapisi: kilitli gorunen hedef gercekten atilabilir olandir.
static bool TrackSelected(const Vec3& cpos, const Vec3& cfwd) {
    auto& st = State();
    void* target = selectedTarget;
    if (!target || !IsHuntTarget(target)) return false;
    float maxD = st.silentMaxDist;
    if (maxD < 5.f) maxD = 5.f;
    if (maxD > 800.f) maxD = 800.f;
    float fov = st.silentFov;
    if (fov < 1.f) fov = 1.f;
    if (fov > 80.f) fov = 80.f;
    Vec3 center, extents;
    if (!ValidateTarget(target, cpos, cfwd, fov * 0.5f, maxD, center, extents, true)) return false;
    Vec3 to{ center.x - cpos.x, center.y - cpos.y, center.z - cpos.z };
    float ds = to.x * to.x + to.y * to.y + to.z * to.z;
    PublishSnapshot(target, cpos, cfwd, center, extents, ds, TargetKind(target));
    return true;
}

void TickPreview() {
    auto& st = State();
    if (!st.silentAim) {
        st.espValid = false;
        selectedTarget = nullptr;
        return;
    }
    ULONGLONG now = GetTickCount64();
    // Gercek Player.Update hook'u varsa oyunun kendi frame hizini izle; yapay 60 Hz
    // siniri internal ESP'yi external/interpolasyonlu gibi gecikmeli hissettiriyordu.
    if (frameHooked) {
        if (now == lastPreviewMs) return; // ayni milisaniyedeki cift Update'i ele
    } else if (now - lastPreviewMs < 16) return; // seyrek fallback icin koruma
    lastPreviewMs = now;
    if (InterlockedCompareExchange(&inPreview, 1, 0) != 0) return;
    __try {
        Vec3 cpos, cfwd;
        if (CamPose(cpos, cfwd)) {
            bool tracked = false;
            if (now - lastFullScanMs < 100) tracked = TrackSelected(cpos, cfwd);
            if (!tracked || now - lastFullScanMs >= 100) {
                lastFullScanMs = now;
                SelectAndPublish(cpos, cfwd);
            }
        } else st.espValid = false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st.espValid = false;
        selectedTarget = nullptr;
    }
    InterlockedExchange(&inPreview, 0);
}

static void __cdecl HookPlayerUpdate(void* self) {
    if (oPlayerUpdate) oPlayerUpdate(self);
    __try {
        // Kuyruk LocalPlayer karsilastirmasina bagli degil; ilk Player frame'inde islenir.
        fishspawn::Tick();
        void* lp = LocalPlayer();
        if (self && lp && (self == lp || SamePlayer(self, lp))) {
            // Unity/physics degerini kendi Update'i yazdiktan sonra uygula.
            // Arka plan thread'indeki 50ms yazimin hemen ezilmesi AirJump'i bozuyordu.
            cheats::AirJumpTick();
            cheats::NoReloadFrameTick();
            TickPreview();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

// Ates aninda cagrilir: hedef varsa FirePoint'i dondur + spread sifirla.
// Donus: restore gerekli mi?
static bool AimAndSave(void* self, ShotPlan& plan) {
    auto& st = State();
    auto& mono = MonoAPI::Get();
    if (!st.silentAim || !self) return false;
    st.espValid = false; // hedef bulunamazsa ESP de temizlenir
    if (!EnsureClasses() || !EnsureMethods()) return false;

    // holder yerel oyuncu mu?
    MonoClass* wc = mono.ObjectClass(self);
    if (!wc) return false;
    uint32_t hoff = 0;
    void* holder = nullptr;
    if (!mono.GetFieldOffset(wc, "_holder", hoff) || !RP(self, hoff, holder) || !holder) return false;
    void* lp = LocalPlayer();
    if (!lp || !SamePlayer(holder, lp)) return false;

    // kamera
    Vec3 cpos, cfwd;
    if (!CamPose(cpos, cfwd)) return false;

    // sniper nisangahindaysa kamera zaten hassas -> dokunma
    {
        uint32_t ao = 0, ar = 0;
        if (mono.GetFieldOffset(wc, "_aimPercent", ao) && R32(self, ao, ar)) {
            if (R2F(ar) > 0.9f) {
                void* atts = nullptr;
                uint32_t toff = 0;
                if (mono.GetFieldOffset(wc, "_attachments", toff) && RP(self, toff, atts) && atts) {
                    MonoClass* ac = mono.ObjectClass(atts);
                    MonoMethod* gu = ac ? mono.FindMethod(ac, "get_UseSniperUi", 0) : nullptr;
                    if (gu) {
                        bool u = false;
                        if (UnBool(mono.Invoke(gu, atts, nullptr), u) && u) return false;
                    }
                }
            }
        }
    }

    // Ates aninda bir kez daha yenile; gorulen ESP hedefi ile merminin hedefi aynidir.
    if (!SelectAndPublish(cpos, cfwd)) return false;
    void* best = selectedTarget;
    Vec3 bestC = selectedCenter;
    float bestD = selectedDistSq;

    // FirePoint coz
    void* atts = nullptr;
    {
        uint32_t toff = 0;
        if (!mono.GetFieldOffset(wc, "_attachments", toff) || !RP(self, toff, atts) || !atts) return false;
    }
    MonoClass* ac = mono.ObjectClass(atts);
    if (!ac) return false;
    MonoMethod* gfp = mono.FindMethod(ac, "get_FirePoint", 0);
    if (!gfp) return false;
    void* fp = mono.Invoke(gfp, atts, nullptr);
    if (!fp) return false;
    // FirePoint pozisyonu + yon
    MonoMethod* gp = Meth0(cTransform, "get_position");
    MonoMethod* gr = Meth0(cTransform, "get_rotation");
    MonoMethod* sf = Meth0(cTransform, "set_forward");
    MonoMethod* sr = Meth0(cTransform, "set_rotation");
    if (!gp || !gr || !sf || !sr) return false;
    Vec3 fpos;
    if (!UnVec(mono.Invoke(gp, fp, nullptr), fpos)) return false;
    Quat sq;
    if (!UnQuat(mono.Invoke(gr, fp, nullptr), sq)) return false;
    // Hizli hedefler (Albatross) icin ondeleme: hedef hizi x mermi gidis suresi.
    // silentBulletSpeed=0 ise lead kapali (hitscan silahlarda oldugu gibi birakir).
    Vec3 aim = bestC;
    float distM = sqrtf(bestD);
    if (st.silentBulletSpeed > 1.f && trackValid && best == trackTarget) {
        float t = distM / st.silentBulletSpeed;
        if (t > 0.f && t < 3.f) {
            aim.x += trackVel.x * t;
            aim.y += trackVel.y * t;
            aim.z += trackVel.z * t;
        }
    }
    Vec3 dir{ aim.x - fpos.x, aim.y - fpos.y, aim.z - fpos.z };
    float dl = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dl < 1e-4f) return false;
    // Namlu LoS: kamera hedefi goruyor diye namlu da goruyor degildir
    // (kaya kosesi vb.). Namlu blokluysa yonlendirme yapma, mermi duz gider.
    if (!HasLineOfSight(fpos, aim, selectedExtents)) {
        static bool s_mzlLogged = false;
        if (!s_mzlLogged) {
            s_mzlLogged = true;
            st.Log("[!] Namlu bloklu: yonlendirme iptal (acini degistir).");
        }
        return false;
    }
    dir.x /= dl; dir.y /= dl; dir.z /= dl;
    void* fa[1] = { &dir };
    mono.Invoke(sf, fp, fa); // FirePoint hedefe dondu (kamera sabit!)
    // spread sifirla (sonra geri alinir)
    uint32_t soff = 0;
    float sspread = 0.f;
    if (mono.GetFieldOffset(wc, "_spread", soff)) {
        uint32_t srw = 0;
        if (R32(self, soff, srw)) {
            sspread = R2F(srw);
            RW32(self, soff, 0);
        } else return false;
    }
    plan.redirect = true;
    plan.fp = fp;
    plan.savedQ = sq;
    plan.spread = sspread;
    plan.spreadOff = soff;
    plan.wcls = wc;
    ++State().silentShots; // tani sayaci: sadece gercek yonlendirmede artar
    // ESP paylasimi: ayni hedef cizimde de kullanilir
    st.espTx = bestC.x; st.espTy = bestC.y; st.espTz = bestC.z;
    st.espCx = cpos.x; st.espCy = cpos.y; st.espCz = cpos.z;
    st.espFx = cfwd.x; st.espFy = cfwd.y; st.espFz = cfwd.z;
    st.espDist = sqrtf(bestD);
    st.espFovY = CamFovY();
    st.espValid = true;
    {
        // Yeni hedefe kilitlenince bir kere logla (spam yok).
        // Tur + mesafe + hiz: Albatross'a ates yonlendi mi buradan belli olur.
        static void* lastLogged = nullptr;
        if (best != lastLogged) {
            lastLogged = best;
            int kind = TargetKind(best);
            const char* kn = kind == 3 ? "ALBATROSS" : (kind == 2 ? "BIRD" : (kind == 1 ? "FISH" : "?"));
            float spd = trackValid ? sqrtf(trackVel.x * trackVel.x + trackVel.y * trackVel.y + trackVel.z * trackVel.z) : -1.f;
            char lb[160];
            if (spd >= 0.f) sprintf_s(lb, "[+] Ates yonlendi: %s %.0fm (hiz %.1f m/s)", kn, (double)st.espDist, (double)spd);
            else sprintf_s(lb, "[+] Ates yonlendi: %s %.0fm", kn, (double)st.espDist);
            st.Log(lb);
        }
    }
    return true;
}

static void Restore(const ShotPlan& plan) {
    if (!plan.redirect || !plan.fp) return;
    __try {
        MonoMethod* sr = Meth0(cTransform, "set_rotation");
        if (sr) {
            Quat q = plan.savedQ;
            void* qa[1] = { &q };
            MonoAPI::Get().Invoke(sr, plan.fp, qa);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

static void __cdecl HookShoot(void* self) {
    if (InterlockedCompareExchange(&inShoot, 1, 0) != 0) {
        if (oShoot) oShoot(self);
        return;
    }
    NoRecoilPlan recoilPlan;
    __try { ApplyNoRecoilForShot(self, recoilPlan); }
    __except (EXCEPTION_EXECUTE_HANDLER) { recoilPlan = {}; }
    ShotPlan plan;
    bool armed = false;
    __try { armed = AimAndSave(self, plan); }
    __except (EXCEPTION_EXECUTE_HANDLER) { armed = false; }
    InterlockedExchange(&inShoot, 0);
    if (oShoot) oShoot(self);
    // Geri al (orijinal Shoot bittikten sonra; ayri __try yok, Restore kendi korur)
    if (armed) {
        auto& mono = MonoAPI::Get();
        if (plan.spreadOff && plan.wcls) {
            uint32_t sraw = 0;
            memcpy(&sraw, &plan.spread, 4);
            RW32(self, plan.spreadOff, sraw);
        }
        Restore(plan);
        (void)mono;
    }
    // Atis hesaplari bittikten sonra silahin prefab/instance degerlerini bozma.
    __try { RestoreNoRecoilAfterShot(self, recoilPlan); }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}

bool Install() {
    auto& s = State();
    auto& mono = MonoAPI::Get();
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false;
    if (!EnsureClasses()) { s.Log("[!] SilentAim: siniflar bulunamadi."); return false; }
    EnsureMethods(); // LoS imza logu icin erken coz (basarisizsa sonra tekrar denenecek)
    MonoClass* wcls = mono.FindClass("", "Weapon");
    if (!wcls) { s.Log("[!] SilentAim: Weapon yok."); return false; }
    MonoMethod* shoot = mono.FindMethod(wcls, "Shoot", 0);
    if (!shoot) shoot = mono.FindMethodAny(wcls, "Shoot");
    if (!shoot) { s.Log("[!] SilentAim: Shoot bulunamadi."); return false; }
    void* code = mono.CompileMethod(shoot);
    if (!code) { s.Log("[!] SilentAim: Shoot JIT alinamadi."); return false; }
    {
        char ab[96];
        sprintf_s(ab, ">> Shoot JIT: 0x%p", code);
        s.Log(ab);
    }
    MH_STATUS hc = MH_CreateHook(code, &HookShoot, (void**)&oShoot);
    if (hc != MH_OK) {
        char hb[128];
        sprintf_s(hb, "[!] SilentAim: hook kurulamadi (MinHook:%d)", (int)hc);
        s.Log(hb);
        return false;
    }
    if (MH_EnableHook(code) != MH_OK) {
        s.Log("[!] SilentAim: hook acilamadi."); return false;
    }
    hooked = true;
    MonoMethod* addRecoil = mono.FindMethod(wcls, "AddModelRecoil", 0);
    void* recoilCode = addRecoil ? mono.CompileMethod(addRecoil) : nullptr;
    if (recoilCode && MH_CreateHook(recoilCode, &HookAddModelRecoil, (void**)&oAddModelRecoil) == MH_OK &&
        MH_EnableHook(recoilCode) == MH_OK) {
        s.Log("[+] No Recoil hook hazir (spread + kick + model).");
    } else {
        s.Log("[!] AddModelRecoil hook kurulamadi; spread/kick yine sifirlanacak.");
    }
    MonoClass* cameraClass = mono.FindClass("", "PlayerCamera");
    MonoClass* toolClass = mono.FindClass("", "PlayerToolMovement");
    MonoMethod* cameraRecoil = cameraClass ? mono.FindMethod(cameraClass, "Recoil", 1) : nullptr;
    MonoMethod* toolRecoil = toolClass ? mono.FindMethod(toolClass, "Recoil", 1) : nullptr;
    MonoMethod* cameraMouse = cameraClass ? mono.FindMethod(cameraClass, "MouseMovement", 0) : nullptr;
    void* cameraCode = cameraRecoil ? mono.CompileMethod(cameraRecoil) : nullptr;
    void* toolCode = toolRecoil ? mono.CompileMethod(toolRecoil) : nullptr;
    void* cameraMouseCode = cameraMouse ? mono.CompileMethod(cameraMouse) : nullptr;
    bool cameraOk = cameraCode && MH_CreateHook(cameraCode, &HookCameraRecoil, (void**)&oCameraRecoil) == MH_OK &&
                    MH_EnableHook(cameraCode) == MH_OK;
    bool toolOk = toolCode && MH_CreateHook(toolCode, &HookToolRecoil, (void**)&oToolRecoil) == MH_OK &&
                  MH_EnableHook(toolCode) == MH_OK;
    bool cameraStateOk = cameraMouseCode &&
        MH_CreateHook(cameraMouseCode, &HookCameraMouseMovement, (void**)&oCameraMouseMovement) == MH_OK &&
        MH_EnableHook(cameraMouseCode) == MH_OK;
    if (cameraOk && toolOk && cameraStateOk)
        s.Log("[+] Kamera + el recoil hook hazir; eski kamera recoil state temizleniyor.");
    else
        s.Log("[!] Kamera/el/recoil-state hook eksik; logu paylas.");
    // Gercek frame tick: MoneyManager.Update seyrek calisabildigi icin ESP 1 FPS gorunuyordu.
    MonoMethod* pupd = mono.FindMethod(cPlayer, "LateUpdate", 0);
    if (!pupd) pupd = mono.FindMethod(cPlayer, "Update", 0);
    void* pcode = pupd ? mono.CompileMethod(pupd) : nullptr;
    if (pcode && MH_CreateHook(pcode, &HookPlayerUpdate, (void**)&oPlayerUpdate) == MH_OK &&
        MH_EnableHook(pcode) == MH_OK) {
        frameHooked = true;
        s.Log("[+] ESP frame hook aktif (native frame takip / 10 Hz tam tarama).");
    } else {
        s.Log("[!] ESP frame hook yok; MoneyManager.Update yedegi kullanilacak.");
    }
    s.Log("[+] Silent Aim hazir (F8 acar, FOV 15). Hip-fire calisir, sniper'da pasif.");
    {
        // LoS overload imzasi tani icin: (17,17,12) olmali.
        char sg[96];
        sprintf_s(sg, ">> Raycast imza: (%d,%d,%d)%s", mono.ParamTypeCode(mRaycast, 0),
            mono.ParamTypeCode(mRaycast, 1), mono.ParamTypeCode(mRaycast, 2),
            mRaycast ? "" : " [YOK: LoS kontrolsuz]");
        s.Log(sg);
    }
    s.Log(cAlbatross ? "[+] Albatross hedefte (Fish/Bird/Albatross)."
                     : "[!] Albatross sinifi cozulemedi; bilinmeyen Creature'lar fallback ile hedeflenecek.");
    return true;
}

bool IsHooked() { return hooked; }
bool HasFrameHook() { return frameHooked; }
} // namespace silentaim
