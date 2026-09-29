// Casino hileleri. Bkz. casino.h
// Katman 1 (birincil): FIZIKSEL rig - top yavaslayinca wheel hedef rengin
// altina dondurulur. Oyun kendi GetRouletteColorFromBall() ile dogru sonucu
// okur, normal odeme akisi calisir.
// Katman 2 (yedek): ServerRouletteResult(winColor) argumani oyuncunun
// bahsine esitlenir (wheel tutmazsa bile kazanilir).
// Carpi: Item.AddBetMultiplier(mult) argumani casinoMult ile carpilir.
#include "casino.h"
#include "cheats.h"
#include "mono_api.h"
#include "memory.h"
#include "MinHook.h"
#include <windows.h>
#include <cstring>
#include <cmath>

namespace casino {
typedef void (__cdecl* SRFn)(void*, int);
typedef void (__cdecl* ABMFn)(void*, float);
typedef void (__cdecl* URFN)(void*);
static SRFn oSR = nullptr;
static ABMFn oABM = nullptr;
static URFN oUR = nullptr;
static bool hooked = false;

// Transform/Rigidbody yardimcilari (main thread)
static MonoClass* cTransform = nullptr;
static MonoMethod *mGetEuler = nullptr, *mSetEuler = nullptr, *mLookAtT = nullptr,
                  *mGetLinVel = nullptr;

struct Vec3 { float x, y, z; };
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

// 1 parametreli + parametre tipi verilmis overload sec (LookAt(Transform) vs LookAt(Vector3))
static MonoMethod* FindMethod1Typed(MonoClass* klass, const char* name, int wantCode) {
    auto& mono = MonoAPI::Get();
    MonoMethod* found = nullptr;
    mono.ForEachMethod(klass, [&](MonoMethod* m) {
        if (found) return;
        const char* n = mono.MethodName(m);
        if (!n || strcmp(n, name) != 0) return;
        if (mono.ParamCount(m) != 1) return;
        if (mono.ParamTypeCode(m, 0) != wantCode) return;
        found = m;
    });
    return found;
}

static bool EnsureT() {
    auto& mono = MonoAPI::Get();
    if (!cTransform) cTransform = mono.FindClass("UnityEngine", "Transform");
    if (!cTransform) return false;
    if (!mGetEuler) mGetEuler = mono.FindMethod(cTransform, "get_eulerAngles", 0);
    if (!mSetEuler) mSetEuler = mono.FindMethod(cTransform, "set_eulerAngles", 1);
    if (!mLookAtT) mLookAtT = FindMethod1Typed(cTransform, "LookAt", 18); // Transform (class)
    if (!mGetLinVel) {
        MonoClass* rb = mono.FindClass("UnityEngine", "Rigidbody");
        if (rb) mGetLinVel = mono.FindMethod(rb, "get_linearVelocity", 0);
    }
    return mGetEuler && mSetEuler && mLookAtT && mGetLinVel;
}

static bool EulerY(void* tr, float& y) {
    auto& mono = MonoAPI::Get();
    void* b = mono.Invoke(mGetEuler, tr, nullptr);
    Vec3 e;
    if (!UnVec(b, e)) return false;
    y = e.y;
    return true;
}

// BetColor enum degeri (0=Black 1=Red 2=Green) -> slot indexi (0=Green 1=Black 2=Red)
static int SlotForColor(int betColor) {
    switch (betColor) {
        case 2: return 0; // Green
        case 0: return 1; // Black
        case 1: return 2; // Red
        default: return -1;
    }
}

static void ForceWheelToColor(void* inst, int betColor) {
    auto& mono = MonoAPI::Get();
    int targetSlot = SlotForColor(betColor);
    if (targetSlot < 0) return;
    uint32_t off = 0;
    void *bao = nullptr, *wh = nullptr;
    if (!mono.GetFieldOffset(mono.ObjectClass(inst), "_ballAngleObject", off) || !RP(inst, off, bao) || !bao) return;
    if (!mono.GetFieldOffset(mono.ObjectClass(inst), "_wheel", off) || !RP(inst, off, wh) || !wh) return;
    // ayni aci sistemiyle bak: _ballAngleObject -> _wheel
    {
        void* la[1] = { wh };
        mono.Invoke(mLookAtT, bao, la);
    }
    float ballAngle = 0.f, wheelAngle = 0.f, slotSize = 9.72973f;
    if (!EulerY(bao, ballAngle)) return;
    if (!EulerY(wh, wheelAngle)) return;
    {
        float ss = 0.f;
        uint32_t so = 0;
        uint32_t raw = 0;
        if (mono.GetFieldOffset(mono.ObjectClass(inst), "_slotSize", so) && R32(inst, so, raw)) {
            float f;
            memcpy(&f, &raw, 4);
            if (f > 1.f && f < 30.f) ss = f;
        }
        if (ss > 0.f) slotSize = ss;
    }
    float desired = ((float)targetSlot + 0.5f) * slotSize;
    float newWheelY = ballAngle - desired;
    // x,z'yi koru, sadece y yaz
    void* be = mono.Invoke(mGetEuler, wh, nullptr);
    Vec3 e;
    if (!UnVec(be, e)) return;
    e.y = newWheelY;
    void* sa[1] = { &e };
    mono.Invoke(mSetEuler, wh, sa);
}

static void __cdecl HookUR(void* self) {
    if (State().casinoWin && self) {
        __try {
            auto& mono = MonoAPI::Get();
            MonoClass* lc = mono.ObjectClass(self);
            if (lc) {
                void* inst = self;
                // _isSpinning kontrolu (plain bool field; yoksa devam)
                uint32_t so = 0;
                bool spinning = true;
                uint8_t sv = 1;
                if (mono.GetFieldOffset(lc, "_isSpinning", so)) {
                    __try { sv = *(uint8_t*)((uint8_t*)inst + so); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { sv = 1; }
                    spinning = (sv != 0);
                }
                if (spinning) {
                    // top hizi
                    void* ball = nullptr;
                    uint32_t bo = 0;
                    float speed = 999.f;
                    if (mono.GetFieldOffset(lc, "_ball", bo) && RP(inst, bo, ball) && ball) {
                        void* bv = mono.Invoke(mGetLinVel, ball, nullptr);
                        Vec3 vv;
                        if (UnVec(bv, vv))
                            speed = sqrtf(vv.x * vv.x + vv.y * vv.y + vv.z * vv.z);
                    }
                    if (speed < 0.5f) {
                        // hedef renk: menudeki secim (0=Black 1=Red 2=Green)
                        int target = State().casinoColor;
                        if (target < 0 || target > 2) target = 1;
                        ForceWheelToColor(inst, target);
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    if (oUR) oUR(self);
}

static void __cdecl HookSR(void* self, int winColor) {
    int use = winColor;
    if (State().casinoWin) {
        auto& mono = MonoAPI::Get();
        MonoClass* cm = mono.FindClass("", "CasinoManager");
        if (cm) {
            int bet = 0;
            // BetColor: 0=Black 1=Red 2=Green (int tabanli enum)
            if (mono.GetStaticInt(cm, "_curBetColor", bet) && bet >= 0 && bet <= 2)
                use = bet;
        }
    }
    if (oSR) oSR(self, use);
}

static void __cdecl HookABM(void* self, float mult) {
    float use = mult;
    float f = State().casinoMult;
    if (f > 1.01f && mult > 0.f && mult < 100000.f) use = mult * f;
    if (oABM) oABM(self, use);
}

// ---------- Slot debug ----------
// Slot forcing ARTIK oyun tarafinda: patch'li Assembly-CSharp.dll icindeki
// RollRandom() rolled'u kendisi secer (sari-test: rolled=(byte)num3).
// Native SendRoll/Roll hook + rarity lookup yollari KALDIRILDI.
// Menu sadece SetForcedResult() invoke eder (secici patch'inde aktif olur).
bool Install() {
    auto& s = State();
    auto& mono = MonoAPI::Get();
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) return false;
    if (!EnsureT()) s.Log("[!] Casino: Transform API bulunamadi (fiziksel rig pasif).");
    bool okSR = false, okABM = false, okUR = false;
    MonoClass* cm = mono.FindClass("", "CasinoManager");
    if (cm) {
        MonoMethod* m = mono.FindMethod(cm, "ServerRouletteResult", 1);
        if (!m) m = mono.FindMethodAny(cm, "ServerRouletteResult");
        void* code = m ? mono.CompileMethod(m) : nullptr;
        if (code && MH_CreateHook(code, &HookSR, (void**)&oSR) == MH_OK &&
            MH_EnableHook(code) == MH_OK) okSR = true;
    }
    MonoClass* it = mono.FindClass("", "Item");
    if (it) {
        MonoMethod* m = mono.FindMethod(it, "AddBetMultiplier", 1);
        if (!m) m = mono.FindMethodAny(it, "AddBetMultiplier");
        void* code = m ? mono.CompileMethod(m) : nullptr;
        if (code && MH_CreateHook(code, &HookABM, (void**)&oABM) == MH_OK &&
            MH_EnableHook(code) == MH_OK) okABM = true;
    }
    MonoClass* lc = mono.FindClass("", "LocalCasino");
    if (lc && EnsureT()) {
        MonoMethod* m = mono.FindMethod(lc, "ServerUpdateRouletteGame", 0);
        if (!m) m = mono.FindMethodAny(lc, "ServerUpdateRouletteGame");
        void* code = m ? mono.CompileMethod(m) : nullptr;
        if (code && MH_CreateHook(code, &HookUR, (void**)&oUR) == MH_OK &&
            MH_EnableHook(code) == MH_OK) okUR = true;
    }
    if (okSR) s.Log("[+] Casino sonuc yedegi hazir (F9)");
    else s.Log("[!] ServerRouletteResult hook kurulamadi.");
    if (okABM) s.Log("[+] Casino carpi hazir (slider)");
    else s.Log("[!] AddBetMultiplier hook kurulamadi.");
    if (okUR) s.Log("[+] Casino fiziksel rig hazir (top yavaslayinca wheel hedefe doner)");
    else s.Log("[!] ServerUpdateRouletteGame hook kurulamadi.");
    hooked = okSR || okABM || okUR;
    return hooked;
}

bool IsHooked() { return hooked; }
} // namespace casino
