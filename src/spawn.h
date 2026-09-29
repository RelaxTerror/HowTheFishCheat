#pragma once
// Balik spawn: Fishable asset'inden prefab -> Instantiate -> server Spawn.
// Istek F10/menu ile kuyruga girer, main thread'de (Update hook) islenir.
// Secenekler: balik turu (Cod/Triggerfish), shiny (SetDrip), olu (ServerKillOnSpawn).
namespace spawn {
void Request(); // F10: State'deki secimle kuyruga ekle
void Tick();    // HookUpdate detour'undan cagrilir (main thread)
} // namespace spawn
