#include "gui.h"
#include "cheats.h"
#include "auth.h"
#include "renderer.h"
#include "silentaim.h"
#include "fishspawn.h"
#include "slot.h"
#include "imgui.h"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <cmath>

namespace gui {

struct Toast { std::string text; bool enabled; ULONGLONG born; };
static std::vector<Toast> g_toasts;
static std::mutex g_toastMtx;

void Notify(const char* feature, bool enabled) {
    if (!feature || !*feature) return;
    std::lock_guard<std::mutex> lock(g_toastMtx);
    g_toasts.push_back({ feature, enabled, GetTickCount64() });
    if (g_toasts.size() > 5) g_toasts.erase(g_toasts.begin());
}

// Modern koyu tema (bir kere uygulanir)
static void ApplyModernStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 10.f;
    st.ChildRounding = 8.f;
    st.FrameRounding = 6.f;
    st.GrabRounding = 6.f;
    st.PopupRounding = 8.f;
    st.ScrollbarRounding = 8.f;
    st.WindowPadding = ImVec2(12, 10);
    st.FramePadding = ImVec2(8, 5);
    st.ItemSpacing = ImVec2(8, 6);
    st.ItemInnerSpacing = ImVec2(6, 4);
    st.WindowBorderSize = 1.f;
    st.FrameBorderSize = 0.f;
    ImVec4* c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.11f, 0.96f);
    c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.11f, 0.15f, 1.00f);
    c[ImGuiCol_Border] = ImVec4(1.00f, 0.45f, 0.10f, 0.55f); // turuncu cerceve
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.18f, 0.23f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.22f, 0.24f, 0.31f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.28f, 0.30f, 0.38f, 1.00f);
    c[ImGuiCol_TitleBg] = ImVec4(0.90f, 0.38f, 0.08f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(1.00f, 0.45f, 0.10f, 1.00f);
    c[ImGuiCol_CheckMark] = ImVec4(1.00f, 0.55f, 0.15f, 1.00f);
    c[ImGuiCol_SliderGrab] = ImVec4(1.00f, 0.50f, 0.12f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.00f, 0.60f, 0.20f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(0.90f, 0.40f, 0.09f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(1.00f, 0.50f, 0.15f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.80f, 0.34f, 0.07f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.90f, 0.42f, 0.10f, 0.85f);
    c[ImGuiCol_HeaderHovered] = ImVec4(1.00f, 0.50f, 0.15f, 0.90f);
    c[ImGuiCol_Text] = ImVec4(0.92f, 0.93f, 0.96f, 1.00f);
}

} // namespace gui

namespace widgets {
void Section(const char* title) {
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.15f, 1.0f), "%s", title);
    ImGui::Separator();
}
void Hint(const char* txt) {
    ImGui::TextDisabled("%s", txt);
}
} // namespace widgets

namespace gui {

// Log satirlarini tek metin yap (kilitli kopya)
static std::string BuildLogText() {
    auto& s = State();
    std::string out;
    std::lock_guard<std::mutex> l(s.logMtx);
    for (auto& line : s.log) { out += line; out += "\r\n"; }
    return out;
}

static bool CopyToClipboard(const std::string& txt) {
    if (txt.empty()) return false;
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, txt.size() + 1);
    if (!h) { CloseClipboard(); return false; }
    memcpy(GlobalLock(h), txt.c_str(), txt.size() + 1);
    GlobalUnlock(h);
    SetClipboardData(CF_TEXT, h);
    CloseClipboard();
    return true;
}

// Font Awesome gerektirmeyen vektor sidebar ikonu. Boylece oyunun varsayilan
// ImGui fontunda emoji/icon glyph'i olmasa bile ikonlar her zaman gorunur.
static bool SidebarItem(const char* id, const char* label, int icon, bool active, float h) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 sz(ImGui::GetContentRegionAvail().x, h);
    ImGui::PushID(id);
    bool clicked = ImGui::InvisibleButton("##nav", sz);
    bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = active ? IM_COL32(244, 92, 20, 255)
                      : (hovered ? IM_COL32(49, 52, 66, 255) : IM_COL32(31, 34, 45, 255));
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), bg, 7.f);
    if (active) dl->AddRectFilled(p, ImVec2(p.x + 3.f, p.y + sz.y), IM_COL32(255, 196, 80, 255), 7.f);

    ImVec2 c(p.x + 20.f, p.y + h * 0.5f);
    ImU32 ic = active ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 145, 55, 255);
    if (icon == 0) { // oyuncu
        dl->AddCircle(ImVec2(c.x, c.y - 5), 4.f, ic, 16, 1.8f);
        dl->AddBezierCubic(ImVec2(c.x - 7, c.y + 8), ImVec2(c.x - 6, c.y + 1),
            ImVec2(c.x + 6, c.y + 1), ImVec2(c.x + 7, c.y + 8), ic, 1.8f);
    } else if (icon == 1) { // aim/crosshair
        dl->AddCircle(c, 7.f, ic, 20, 1.6f);
        dl->AddCircleFilled(c, 2.f, ic, 10);
        dl->AddLine(ImVec2(c.x - 11, c.y), ImVec2(c.x - 6, c.y), ic, 1.6f);
        dl->AddLine(ImVec2(c.x + 6, c.y), ImVec2(c.x + 11, c.y), ic, 1.6f);
        dl->AddLine(ImVec2(c.x, c.y - 11), ImVec2(c.x, c.y - 6), ic, 1.6f);
        dl->AddLine(ImVec2(c.x, c.y + 6), ImVec2(c.x, c.y + 11), ic, 1.6f);
    } else if (icon == 2) { // coin
        dl->AddCircle(c, 9.f, ic, 20, 1.7f);
        ImVec2 ts = ImGui::CalcTextSize("$");
        dl->AddText(ImVec2(c.x - ts.x * .5f, c.y - ts.y * .5f), ic, "$");
    } else if (icon == 3) { // casino chip
        dl->AddCircle(c, 9.f, ic, 8, 1.7f);
        dl->AddCircle(c, 4.f, ic, 12, 1.5f);
        dl->AddCircleFilled(c, 1.5f, ic, 8);
    } else if (icon == 4) { // fish/spawn
        dl->AddEllipse(c, ImVec2(8.f, 5.f), ic, 0.f, 20, 1.7f);
        dl->AddTriangle(ImVec2(c.x - 7, c.y), ImVec2(c.x - 12, c.y - 5), ImVec2(c.x - 12, c.y + 5), ic, 1.7f);
        dl->AddCircleFilled(ImVec2(c.x + 4, c.y - 1), 1.2f, ic, 8);
    } else { // info
        dl->AddCircle(c, 9.f, ic, 20, 1.7f);
        dl->AddCircleFilled(ImVec2(c.x, c.y - 4), 1.3f, ic, 8);
        dl->AddLine(ImVec2(c.x, c.y - 1), ImVec2(c.x, c.y + 5), ic, 2.f);
    }
    ImVec2 tp(p.x + 42.f, p.y + (sz.y - ImGui::GetTextLineHeight()) * .5f);
    dl->AddText(tp, active ? IM_COL32_WHITE : IM_COL32(220, 223, 232, 255), label);
    return clicked;
}

static std::string AsciiTR(const std::string& s) {
    // Varsayilan ImGui fontunda TR glif yok (? cikar), o yuzden cevir.
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        if (c < 0x80) { o += (char)c; ++i; continue; }
        if (i + 1 < s.size()) {
            unsigned char d = s[i + 1];
            char r = 0;
            if (c == 0xC4 && d == 0x9F) r = 'g';
            else if (c == 0xC4 && d == 0x9E) r = 'G';
            else if (c == 0xC4 && d == 0xB1) r = 'i';
            else if (c == 0xC4 && d == 0xB0) r = 'I';
            else if (c == 0xC5 && d == 0x9F) r = 's';
            else if (c == 0xC5 && d == 0x9E) r = 'S';
            else if (c == 0xC3 && d == 0xA7) r = 'c';
            else if (c == 0xC3 && d == 0x87) r = 'C';
            else if (c == 0xC3 && d == 0xB6) r = 'o';
            else if (c == 0xC3 && d == 0x96) r = 'O';
            else if (c == 0xC3 && d == 0xBC) r = 'u';
            else if (c == 0xC3 && d == 0x9C) r = 'U';
            if (r) { o += r; i += 2; continue; }
        }
        ++i; // bilinmeyen bayti atla
    }
    return o;
}

static void LoginPage() {
    if (auth::IsAuthed()) {
        auth::LicenseInfo info = auth::Info();
        widgets::Section("Lisans");
        ImGui::Text("Kullanici: %s", AsciiTR(info.username).c_str());
        ImGui::Text("Seviye: %s   Durum: %s", AsciiTR(info.level).c_str(), AsciiTR(info.status).c_str());
        ImGui::Text("Kalan gun: %d", info.days_left);
        ImGui::TextDisabled("Bitis: %s", info.expiresAt.c_str());
        ImGui::TextDisabled("%s", AsciiTR(auth::Status()).c_str());
        if (ImGui::Button("Cikis Yap", ImVec2(220, 32))) auth::Logout();
        widgets::Section("Duyurular");
        std::vector<auth::Notice> items = auth::Notices();
        if (items.empty()) ImGui::TextDisabled("Duyuru yok.");
        for (size_t i = 0; i < items.size(); ++i) {
            ImGui::TextColored(ImVec4(1, .55f, .15f, 1), "%s", AsciiTR(items[i].title).c_str());
            ImGui::TextWrapped("%s", AsciiTR(items[i].body).c_str());
            if (i + 1 < items.size()) ImGui::Separator();
        }
    } else {
        widgets::Section("Lisans Girisi");
        static char keyBuf[64] = { 0 };
        ImGui::SetNextItemWidth(280.f);
        ImGui::InputText("Lisans Anahtari", keyBuf, sizeof(keyBuf),
                         ImGuiInputTextFlags_Password);
        if (ImGui::Button("Giris Yap", ImVec2(220, 36))) auth::LoginAsync(keyBuf);
        ImVec4 sc = auth::IsWorking() ? ImVec4(1, .8f, .3f, 1)
                                      : ImVec4(1, .35f, .3f, 1);
        ImGui::TextColored(sc, "%s", AsciiTR(auth::Status()).c_str());
        ImGui::Spacing();
        ImGui::TextDisabled("HWID: %s", auth::Hwid().c_str());
        widgets::Hint("Anahtar ilk kullanimda bu cihaza kilitlenir.");
    }
}

void Draw() {
    static bool styled = false;
    if (!styled) { ApplyModernStyle(); styled = true; }
    static int page = 5; // varsayilan: Giris
    auto& s = State();
    bool locked = !auth::IsAuthed();
    if (locked) page = 5;
    ImGui::SetNextWindowSize(ImVec2(720, 500), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(650, 430), ImVec2(1000, 760));
    ImGui::Begin("FISH CONTROL  //  v80", nullptr, ImGuiWindowFlags_NoCollapse);

    ImGui::BeginChild("##sidebar", ImVec2(172, 0), true);
    ImGui::TextColored(ImVec4(1.0f, .54f, .16f, 1), "FISH // CONTROL");
    ImGui::TextDisabled("internal suite v80");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    const char* nav[] = { "Oyuncu", "Aim & ESP", "Fish Spawner", "Ekonomi", "Tani / Log", "Giris" };
    const int navIcons[] = { 0, 1, 4, 2, 5, 5 };
    // 6 sekme footer'siz alana her zaman sigsin diye yukseklik dinamik (footer ile cakisma olmaz)
    float footY = ImGui::GetWindowHeight() - 68.f;
    float navH = (footY - ImGui::GetCursorPosY() - 5 * 3.f) / 6.f;
    if (navH > 40.f) navH = 40.f;
    if (navH < 28.f) navH = 28.f;
    for (int i = 0; i < 6; ++i) {
        if (locked && i != 5) continue; // girissiz kullanim yok
        if (SidebarItem(nav[i], nav[i], navIcons[i], page == i, navH)) page = i;
        ImGui::Dummy(ImVec2(0, 3));
    }
    ImGui::SetCursorPosY(footY);
    ImGui::Separator();
    ImGui::TextColored(s.monoReady ? ImVec4(.28f, 1, .48f, 1) : ImVec4(1, .65f, .2f, 1),
        "o  Mono %s", s.monoReady ? "bagli" : "bekleniyor");
    ImGui::TextColored(silentaim::IsHooked() ? ImVec4(.28f, 1, .48f, 1) : ImVec4(1, .3f, .25f, 1),
        "o  Aim hook %s", silentaim::IsHooked() ? "hazir" : "yok");
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##content", ImVec2(0, 0), false);
    const char* titles[] = { "OYUNCU", "AIM & ESP", "FISH SPAWNER", "EKONOMI", "TANI / LOG", "GIRIS" };
    const char* desc[] = { "Hareket ve oyuncu korumalari", "Canli hedef secimi ve gorsellestirme",
        "Oyunun server komutuyla balik olustur", "Para, rulet ve kazanc ayarlari",
        "Sistem durumu ve teknik kayitlar", "Lisans anahtari ile giris" };
    ImGui::TextColored(ImVec4(1, .55f, .15f, 1), "%s", titles[page]);
    ImGui::TextDisabled("%s", desc[page]);
    ImGui::Separator();
    ImGui::Spacing();

    if (s.trainerCount > 1)
        ImGui::TextColored(ImVec4(1, .2f, .2f, 1), "UYARI: %d trainer yuklu - oyunu yeniden baslat.", s.trainerCount);

    if (page == 0) {
        widgets::Section("Koruma");
        if (ImGui::Checkbox("Olumsuzluk  [F1]", &s.godMode)) { cheats::ApplyGod(s.godMode); Notify("Olumsuzluk", s.godMode); }
        if (ImGui::Checkbox("Aclik kilidi  [F2]", &s.noHunger)) { cheats::ApplyHunger(s.noHunger); Notify("Aclik kilidi", s.noHunger); }
        if (ImGui::Checkbox("No Recoil", &s.noRecoil)) { cheats::SetNoRecoil(s.noRecoil); Notify("No Recoil", s.noRecoil); }
        if (ImGui::Checkbox("No Reload / Sonsuz Mermi  [F4]", &s.infAmmo)) { cheats::ApplyAmmo(s.infAmmo); Notify("No Reload", s.infAmmo); }
        if (ImGui::Checkbox("Infinite Jump  [F3 + SPACE]", &s.airJump)) Notify("Infinite Jump", s.airJump);
        widgets::Section("Hareket & Hasar");
        ImGui::Checkbox("Hiz hilesi", &s.speedHack);
        if (s.speedHack) ImGui::SliderFloat("Hiz carpani", &s.speedMult, 1.0f, 5.0f, "%.1fx");
        if (ImGui::Button(("Hasar carpani: " + std::string(DmgLabel(s.dmgIndex)) + "  [F5]").c_str(), ImVec2(250, 34))) {
            cheats::CycleDamage(); Notify(DmgLabel(s.dmgIndex), s.dmgIndex != 0);
        }
    } else if (page == 1) {
        widgets::Section("Silent Aim");
        if (ImGui::Checkbox("Silent Aim etkin  [F8]", &s.silentAim)) {
            if (!s.silentAim) s.espValid = false;
            Notify("Silent Aim", s.silentAim);
        }
        ImGui::SameLine();
        ImGui::TextColored(s.silentAim ? ImVec4(.3f, 1, .45f, 1) : ImVec4(.55f, .57f, .62f, 1),
            s.silentAim ? "LIVE" : "OFF");
        ImGui::SliderFloat("FOV acisi", &s.silentFov, 5.0f, 40.0f, "%.0f derece");
        ImGui::SliderFloat("Maks. mesafe", &s.silentMaxDist, 20.0f, 500.0f, "%.0fm");
        ImGui::SliderFloat("Mermi hizi (lead)", &s.silentBulletSpeed, 0.0f, 300.0f, "%.0f m/s");
        if (s.silentBulletSpeed < 1.f)
            ImGui::TextDisabled("Lead kapali: hizli hedef (Albatross) iskalarsa 60-150 dene.");
        widgets::Section("Gorseller");
        ImGui::Checkbox("ESP master", &s.espEnabled);
        if (s.espEnabled) {
            ImGui::Indent(10.f);
            ImGui::Checkbox("FOV dairesi", &s.espShowFov);
            ImGui::Checkbox("Glow hedef cizgisi", &s.espShowTracer);
            ImGui::Checkbox("Target reticle", &s.espShowReticle);
            ImGui::Checkbox("Balik / kus glow kutusu", &s.espShowEntityBox);
            ImGui::Checkbox("Mesafe etiketi", &s.espShowLabel);
            ImGui::Unindent(10.f);
        }
        if (s.silentAim && s.espValid)
            ImGui::TextColored(ImVec4(.3f, 1, .45f, 1), "o  HEDEF KILITLI   %.0fm", (double)s.espDist);
        else
            ImGui::TextDisabled("o  FOV icinde uygun hedef yok");
        ImGui::TextDisabled("Yonlendirilen atis: %d", s.silentShots);
        widgets::Section("Widget Sistemi");
        ImGui::Checkbox("Hotkey bildirimleri / widget master", &s.hudEnabled);
        ImGui::Checkbox("Aktif ozellikler widget'i", &s.hudActiveList);
        ImGui::Spacing();
        ImGui::TextWrapped("ESP hedefi native oyun frame'inde, interpolasyonsuz izlenir. Ates edildiginde ayni hedef tekrar dogrulanir.");
    } else if (page == 2) {
        static char fishName[64] = "Cod";
        static int spawnCount = 1;
        static bool spawnDead = true;
        static bool spawnDrip = true;
        widgets::Section("Balik Secimi");
        ImGui::SetNextItemWidth(260.f);
        ImGui::InputText("Spawn adi", fishName, sizeof(fishName));
        const char* presets[] = { "Cod", "Pufferfish", "Triggerfish", "Shark", "Piranha" };
        for (int i = 0; i < 5; ++i) {
            if (i) ImGui::SameLine();
            if (ImGui::Button(presets[i])) strcpy_s(fishName, presets[i]);
        }
        widgets::Section("Spawn Ayarlari");
        ImGui::SetNextItemWidth(180.f);
        ImGui::SliderInt("Adet", &spawnCount, 1, 10);
        ImGui::Checkbox("Olu spawn", &spawnDead);
        ImGui::Checkbox("Shiny / Drip", &spawnDrip);
        if (ImGui::Button("BALIGI SPAWN ET", ImVec2(260, 40))) {
            fishspawn::Queue(fishName, spawnCount, spawnDead, spawnDrip);
            Notify("Fish spawn queued", true);
        }
        ImGui::Spacing();
        ImGui::TextWrapped("Host/server tarafinda BaitInfo > Fishable > ItemToSpawn zincirinden gercek balik prefabini bulur. Balik oyuncunun yakininda olusur.");
    } else if (page == 3) {
        widgets::Section("Para Islemleri");
        ImGui::SetNextItemWidth(220);
        ImGui::InputInt("Eklenecek miktar", &s.moneyToAdd, 1000, 10000);
        if (s.moneyToAdd < 0) s.moneyToAdd = 0;
        if (ImGui::Button("Para Ekle  [F6]", ImVec2(220, 36))) cheats::AddMoney(s.moneyToAdd);
        ImGui::Checkbox("Para kilidi", &s.moneyLock);
        if (s.moneyLock) ImGui::TextColored(ImVec4(.3f, 1, .45f, 1), "Kilit degeri: %d", s.moneyLockValue);
        ImGui::Separator();
        widgets::Section("Rulet");
        if (ImGui::Checkbox("Her zaman kazan  [F9]", &s.casinoWin)) Notify("Casino Always-Win", s.casinoWin);
        const char* cols[] = { "Black", "Red", "Green" };
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine();
            ImGui::RadioButton(cols[i], &s.casinoColor, i);
        }
        ImGui::SliderFloat("Kazanc carpani", &s.casinoMult, 1.0f, 100.0f, "%.0fx");
        widgets::Hint("Top yavaslayinca secili renk wheel altina getirilir.");
    } else if (page == 4) {
        widgets::Section("Araclar");
        if (ImGui::Button("Parayi Oku")) cheats::LogMoney();
        ImGui::SameLine();
        if (ImGui::Button("Logu Kopyala")) {
            std::string txt = BuildLogText();
            if (CopyToClipboard(txt)) s.Log("[+] Log panoya kopyalandi.");
            else s.Log("[!] Pano acilamadi.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Tani Dokumu")) cheats::DumpDiagnostics();
        ImGui::BeginChild("log", ImVec2(0, 0), true);
        {
            std::lock_guard<std::mutex> l(s.logMtx);
            for (auto& line : s.log) ImGui::TextWrapped("%s", line.c_str());
            if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
    } else {
        LoginPage();
    }
    ImGui::EndChild();
    ImGui::End();
}

static void DrawHudWidgets(ImDrawList* dl, const ImVec2& scr) {
    auto& s = State();
    if (!s.hudEnabled) return;

    // Hotkey toast'lari: ortada, oyun UI'sini kapatmayacak kompakt kartlar.
    ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_toastMtx);
    for (size_t i = 0; i < g_toasts.size();) {
        ULONGLONG age = now - g_toasts[i].born;
        if (age > 2800) { g_toasts.erase(g_toasts.begin() + i); continue; }
        float fade = age > 2350 ? 1.f - (float)(age - 2350) / 450.f : 1.f;
        int a = (int)(fade * 245.f);
        float w = 250.f, h = 42.f;
        float x = (scr.x - w) * .5f;
        float y = 22.f + (float)i * 48.f;
        ImU32 accent = g_toasts[i].enabled ? IM_COL32(55, 235, 130, a) : IM_COL32(255, 92, 55, a);
        dl->AddRectFilled(ImVec2(x - 4, y - 4), ImVec2(x + w + 4, y + h + 4), IM_COL32(0, 0, 0, (int)(55 * fade)), 10.f);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(17, 20, 28, a), 8.f);
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 4, y + h), accent, 8.f);
        dl->AddCircleFilled(ImVec2(x + 22, y + 21), 5.f, accent, 16);
        dl->AddText(ImVec2(x + 36, y + 8), IM_COL32(238, 241, 247, a), g_toasts[i].text.c_str());
        dl->AddText(ImVec2(x + 36, y + 23), accent, g_toasts[i].enabled ? "ETKIN" : "KAPALI");
        ++i;
    }

    if (!s.hudActiveList) return;
    const char* active[10]; int count = 0;
    if (s.godMode) active[count++] = "Olumsuzluk";
    if (s.noHunger) active[count++] = "Aclik kilidi";
    if (s.airJump) active[count++] = "Infinite Jump";
    if (s.infAmmo) active[count++] = "No Reload";
    if (s.speedHack) active[count++] = "Hiz hilesi";
    if (s.silentAim) active[count++] = "Silent Aim";
    if (s.moneyLock) active[count++] = "Para kilidi";
    if (s.casinoWin) active[count++] = "Casino win";
    if (count == 0) return;

    float w = 178.f, h = 34.f + count * 20.f;
    float x = scr.x - w - 18.f, y = 18.f;
    dl->AddRectFilled(ImVec2(x - 4, y - 4), ImVec2(x + w + 4, y + h + 4), IM_COL32(0, 0, 0, 45), 10.f);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(14, 17, 24, 215), 8.f);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(255, 115, 30, 150), 8.f, 0, 1.f);
    dl->AddText(ImVec2(x + 12, y + 9), IM_COL32(255, 160, 70, 255), "ACTIVE MODULES");
    for (int i = 0; i < count; ++i) {
        float yy = y + 33.f + i * 20.f;
        dl->AddCircleFilled(ImVec2(x + 14, yy + 6), 2.5f, IM_COL32(55, 235, 130, 255), 10);
        dl->AddText(ImVec2(x + 24, yy), IM_COL32(225, 230, 238, 245), active[i]);
    }
}

// Menu kapali olsa da cagrilir. Burada Unity/Mono cagrisi yapilmaz; Shoot hook'unun
// paylastigi kamera ve hedef anlik goruntusu yalnizca ekrana projekte edilir.
void DrawOverlay() {
    auto& s = State();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 scr = ImGui::GetIO().DisplaySize;
    DrawHudWidgets(dl, scr);
    if (s.espEnabled) {
        ImVec2 ctr(scr.x * 0.5f, scr.y * 0.5f);
        float pulse = .5f + .5f * sinf((float)ImGui::GetTime() * 4.5f);
        if (s.silentAim && (s.espShowFov || s.espShowTracer)) {
            // Merkez noktasinda yumusak glow + parlak cekirdek.
            dl->AddCircleFilled(ctr, 8.f, IM_COL32(255, 105, 25, 18), 24);
            dl->AddCircleFilled(ctr, 5.f, IM_COL32(255, 120, 28, 32), 20);
            dl->AddCircleFilled(ctr, 2.2f, IM_COL32(255, 185, 85, 245), 16);
        }
        float fovY = (s.espFovY > 1.f && s.espFovY < 120.f) ? s.espFovY : 60.f;
        float tanHalf = tanf(fovY * 0.5f * 0.0174533f);
        if (tanHalf < 0.05f) tanHalf = 0.05f;
        if (s.silentAim && s.espShowFov) {
            // Uc katmanli FOV glow; tek piksellik ham cember goruntusunu kaldirir.
            float r = tanf(s.silentFov * 0.5f * 0.0174533f) / tanHalf * (scr.y * 0.5f);
            dl->AddCircle(ctr, r, IM_COL32(255, 92, 18, 18), 96, 9.f);
            dl->AddCircle(ctr, r, IM_COL32(255, 108, 24, 42), 96, 4.f);
            dl->AddCircle(ctr, r, IM_COL32(255, 155, 55, 205), 96, 1.35f);
            dl->AddCircle(ctr, r - 2.f, IM_COL32(255, 220, 150, 55), 96, .8f);
        }
        if (s.silentAim && s.espValid) {
            // Sol-elli Unity koordinati: right = worldUp x forward,
            // up2 = forward x right. Tersi hedefi yatayda aynaliyordu.
            float fx = s.espFx, fy = s.espFy, fz = s.espFz;
            float rx = fz, ry = 0.f, rz = -fx; // (0,1,0) x fwd
            float rl = sqrtf(rx * rx + ry * ry + rz * rz);
            if (rl > 1e-6f) {
                rx /= rl; ry /= rl; rz /= rl;
                // up2 = fwd x right
                float ux = fy * rz - fz * ry;
                float uy = fz * rx - fx * rz;
                float uz = fx * ry - fy * rx;
                float tx = s.espTx - s.espCx;
                float ty = s.espTy - s.espCy;
                float tz = s.espTz - s.espCz;
                float zc = tx * fx + ty * fy + tz * fz;
                if (zc > 0.1f) {
                    float xc = tx * rx + ty * ry + tz * rz;
                    float yc = tx * ux + ty * uy + tz * uz;
                    float aspect = scr.x / (scr.y > 1.f ? scr.y : 1.f);
                    float nx = xc / (tanHalf * zc * aspect);
                    float ny = yc / (tanHalf * zc);
                    // Internal ESP: oyun frame'inden gelen koordinati dogrudan kullan.
                    // Interpolasyon yok; hedef modelle birebir, gecikmesiz hareket eder.
                    ImVec2 tp((nx * 0.5f + 0.5f) * scr.x, (1.f - (ny * 0.5f + 0.5f)) * scr.y);

                    // Collider extents'ten balik/kusun ekrandaki yaklasik gercek kutusu.
                    // Baliklar yatay uzun, kuslar daha kompakt oldugu icin oran 3.2x'e kadar korunur.
                    float ey = s.espEy > .08f ? s.espEy : .45f;
                    float ratio = fmaxf(s.espEx, s.espEz) / ey;
                    if (ratio < .60f) ratio = .60f;
                    if (ratio > 3.20f) ratio = 3.20f;
                    float boxH = fabsf((ey * 2.f) / (tanHalf * zc) * (scr.y * .5f));
                    if (boxH < 22.f) boxH = 22.f;
                    if (boxH > 320.f) boxH = 320.f;
                    float boxW = boxH * ratio;
                    ImVec2 box0(tp.x - boxW * .5f, tp.y - boxH * .5f);
                    ImVec2 box1(tp.x + boxW * .5f, tp.y + boxH * .5f);

                    if (s.espShowEntityBox) {
                        bool bird = s.espTargetKind == 2 || s.espTargetKind == 3;
                        ImU32 glow1 = bird ? IM_COL32(255, 120, 70, 20) : IM_COL32(30, 205, 255, 20);
                        ImU32 glow2 = bird ? IM_COL32(255, 145, 75, 65) : IM_COL32(35, 215, 255, 65);
                        ImU32 core  = bird ? IM_COL32(255, 205, 120, 245) : IM_COL32(190, 245, 255, 245);
                        dl->AddRect(box0, box1, glow1, 8.f, 0, 10.f);
                        dl->AddRect(box0, box1, glow2, 7.f, 0, 4.f);
                        dl->AddRect(box0, box1, IM_COL32(15, 20, 28, 120), 6.f, 0, 1.f);
                        float corner = fminf(boxW, boxH) * .24f;
                        if (corner < 8.f) corner = 8.f;
                        if (corner > 22.f) corner = 22.f;
                        // Sadece koseleri parlak ciz: hedefin govdesini kapatmaz.
                        dl->AddLine(box0, ImVec2(box0.x + corner, box0.y), core, 2.f);
                        dl->AddLine(box0, ImVec2(box0.x, box0.y + corner), core, 2.f);
                        dl->AddLine(ImVec2(box1.x, box0.y), ImVec2(box1.x - corner, box0.y), core, 2.f);
                        dl->AddLine(ImVec2(box1.x, box0.y), ImVec2(box1.x, box0.y + corner), core, 2.f);
                        dl->AddLine(ImVec2(box0.x, box1.y), ImVec2(box0.x + corner, box1.y), core, 2.f);
                        dl->AddLine(ImVec2(box0.x, box1.y), ImVec2(box0.x, box1.y - corner), core, 2.f);
                        dl->AddLine(box1, ImVec2(box1.x - corner, box1.y), core, 2.f);
                        dl->AddLine(box1, ImVec2(box1.x, box1.y - corner), core, 2.f);
                    }

                    // Hafif kavisli tracer: genis dusuk-alpha glow katmanlari + beyaz/cyan core.
                    float dx = tp.x - ctr.x, dy = tp.y - ctr.y;
                    float len = sqrtf(dx * dx + dy * dy);
                    float bend = (len * .07f < 34.f) ? len * .07f : 34.f;
                    float px = len > 1.f ? -dy / len : 0.f;
                    float py = len > 1.f ? dx / len : 0.f;
                    ImVec2 c1(ctr.x + dx * .34f + px * bend, ctr.y + dy * .34f + py * bend);
                    ImVec2 c2(ctr.x + dx * .72f + px * bend, ctr.y + dy * .72f + py * bend);
                    if (s.espShowTracer) {
                        dl->AddBezierCubic(ctr, c1, c2, tp, IM_COL32(20, 195, 255, 18), 13.f);
                        dl->AddBezierCubic(ctr, c1, c2, tp, IM_COL32(25, 205, 255, 38), 7.f);
                        dl->AddBezierCubic(ctr, c1, c2, tp, IM_COL32(45, 215, 255, 125), 3.2f);
                        dl->AddBezierCubic(ctr, c1, c2, tp, IM_COL32(220, 250, 255, 245), 1.15f);
                    }

                    // Nabiz atan hedef halkasi ve dort koseli modern reticle.
                    if (s.espShowReticle) {
                        float rr = 10.f + pulse * 3.f;
                        dl->AddCircle(tp, rr + 7.f, IM_COL32(255, 90, 20, 22), 32, 8.f);
                        dl->AddCircle(tp, rr + 2.f, IM_COL32(255, 105, 25, 75), 32, 3.5f);
                        dl->AddCircle(tp, rr, IM_COL32(255, 190, 85, 245), 32, 1.5f);
                        dl->AddCircleFilled(tp, 2.4f, IM_COL32(240, 252, 255, 255), 16);
                        const float a = 17.f, b = 10.f;
                        ImU32 rc = IM_COL32(255, 220, 155, 235);
                        dl->AddLine(ImVec2(tp.x-a,tp.y-a), ImVec2(tp.x-b,tp.y-a), rc, 1.8f);
                        dl->AddLine(ImVec2(tp.x-a,tp.y-a), ImVec2(tp.x-a,tp.y-b), rc, 1.8f);
                        dl->AddLine(ImVec2(tp.x+a,tp.y-a), ImVec2(tp.x+b,tp.y-a), rc, 1.8f);
                        dl->AddLine(ImVec2(tp.x+a,tp.y-a), ImVec2(tp.x+a,tp.y-b), rc, 1.8f);
                        dl->AddLine(ImVec2(tp.x-a,tp.y+a), ImVec2(tp.x-b,tp.y+a), rc, 1.8f);
                        dl->AddLine(ImVec2(tp.x-a,tp.y+a), ImVec2(tp.x-a,tp.y+b), rc, 1.8f);
                        dl->AddLine(ImVec2(tp.x+a,tp.y+a), ImVec2(tp.x+b,tp.y+a), rc, 1.8f);
                        dl->AddLine(ImVec2(tp.x+a,tp.y+a), ImVec2(tp.x+a,tp.y+b), rc, 1.8f);
                    }

                    // Hedefin yaninda okunakli, yuvarlatilmis bilgi etiketi.
                    if (s.espShowLabel) {
                        char buf[64];
                        const char* kind = s.espTargetKind == 3 ? "ALBATROSS" : (s.espTargetKind == 2 ? "BIRD" : (s.espTargetKind == 1 ? "FISH" : "TARGET"));
                        sprintf_s(buf, "%s  %.0fm", kind, (double)s.espDist);
                        ImVec2 textSz = ImGui::CalcTextSize(buf);
                        float lx = box1.x + 8.f, ly = box0.y;
                        if (lx + textSz.x + 18.f > scr.x) lx = tp.x - textSz.x - 41.f;
                        ImVec2 l0(lx, ly), l1(lx + textSz.x + 18.f, ly + 26.f);
                        dl->AddRectFilled(ImVec2(l0.x-3,l0.y-3), ImVec2(l1.x+3,l1.y+3), IM_COL32(15, 190, 240, 20), 8.f);
                        dl->AddRectFilled(l0, l1, IM_COL32(15, 19, 27, 225), 6.f);
                        dl->AddRect(l0, l1, IM_COL32(45, 215, 255, 190), 6.f, 0, 1.2f);
                        dl->AddCircleFilled(ImVec2(lx + 9.f, ly + 13.f), 2.5f, IM_COL32(65, 235, 255, 255), 12);
                        dl->AddText(ImVec2(lx + 15.f, ly + 6.f), IM_COL32(230, 248, 255, 255), buf);
                    }
                }
            }
        }
    }

}

} // namespace gui
