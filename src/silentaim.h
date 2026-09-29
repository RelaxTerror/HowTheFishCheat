#pragma once
// Silent aim: Weapon.Shoot hook (MinHook, JIT adresi).
// Detour oyunun main thread'inde calisir -> Unity cagrilari guvenli.
// Yontem: ates aninda crosshair'a en yakin balik/kus bulunur,
// FirePoint gecici olarak hedefe cevrilir + spread sifirlanir,
// orijinal Shoot cagrilir, sonra ikisi de geri alinir (gorunur snap yok).
namespace silentaim {
bool Install();      // mono + MinHook hazirken bir kere cagir
bool IsHooked();
bool HasFrameHook();
// Oyun main thread'indeki Update hook'undan cagrilir; ates beklemeden ESP hedefini yeniler.
void TickPreview();
} // namespace silentaim
