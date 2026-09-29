#pragma once
namespace gui {
void Draw();        // sadece menu acikken cagrilir
void DrawOverlay(); // menu durumundan bagimsiz, her Present'te cagrilir
void Notify(const char* feature, bool enabled); // hotkey/menu toast bildirimi
} // namespace gui

// Kucuk pencere widget'lari: menu bunlarla kurulur.
namespace widgets {
// Bolum basligi (renkli yazi + cizgi)
void Section(const char* title);
// Gri aciklama satiri
void Hint(const char* txt);
} // namespace widgets
