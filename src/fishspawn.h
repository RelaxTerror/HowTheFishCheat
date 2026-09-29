#pragma once

namespace fishspawn {
// Render thread'den guvenle istek biriktirir.
void Queue(const char* spawnName, int count, bool dead, bool drip);
// Player.Update main thread'inde cagrilir.
void Tick();
}
