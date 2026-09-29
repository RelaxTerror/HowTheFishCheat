#pragma once
// Slot makinesi rig: secili balik icin Legendary skin index'i bulunur,
// SetCheatSkin + RollRandom resmi cheat yoluyla cagrilir.
// Istek F7/menu ile kuyruga girer, main thread'de (Update hook) islenir.
#include <string>
namespace slot {
void Request(const char* fishName); // F7: State disi, gui'deki isimle
void DumpRequest(); // Odul havuzu dokumu istegi (main thread'de calisir)
void Tick();    // HookUpdate detour'undan cagrilir (main thread)
// Casino debug (RollRandom force) icin paylasilan okuyucular, main thread'de cagrilmalidir:
// SkinRarityOf: Item prefab'inin skinIndex'inci skin'inin rarity'si (0=Default 1=Common 2=Rare 3=Legendary), -1=okunamadi
int SkinRarityOf(void* itemObj, int skinIndex);
void ItemNameOf(void* itemObj, char* out, size_t cap);
} // namespace slot
