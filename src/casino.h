#pragma once
// Casino: ServerRouletteResult her zaman oyuncunun bahsine doner (Always Win),
// Item.AddBetMultiplier carpi carpanla buyutulur.
// Slot forcing oyun tarafinda (patch'li RollRandom); menu sadece
// SetForcedResult() invoke eder. Native SendRoll/Roll hook yok.
// Detour'lar server main thread'inde calisir (FishNet host).
namespace casino {
bool Install();      // mono + MinHook hazirken bir kere
bool IsHooked();
} // namespace casino
