#pragma once
#include <windows.h>

// DX11 Present hook + ImGui arka ucu
namespace renderer {
bool Init();      // oyunun swapchain'ini bul, hook kur
void Shutdown();
void ToggleMenuVisible(bool v);
bool IsMenuVisible();
} // namespace renderer
