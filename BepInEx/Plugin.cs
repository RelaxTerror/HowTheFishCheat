// BepInEx 5 alternatifi (Unity Mono icin EN STABIL yol).
// Bu dosya C# - BepInEx plugin olarak derlenir, inject gerekmez.
// Kurulum:
//   1) https://github.com/BepInEx/BepInEx/releases -> BepInEx 5 (x64) indir
//   2) Oyunun klasorune (How to Fish.exe'nin yanina) cikart, oyunu 1 kez ac-kapat
//   3) BepInEx/plugins/HowToFishTrainer.dll olarak bu dosyayi derleyip at
// Derleme: dotnet build (asagidaki csproj ile) veya VS'de Class Library (.NET Framework 4.7.2)
//
// Bu plugin, C++ internal ile ayni hileleri Harmony prefix (RET etkisi) ile yapar:
// TakeDamage vb. metodlara Prefix koyup `return false` => hasar islemez.

using BepInEx;
using HarmonyLib;
using UnityEngine;

namespace HowToFishTrainer
{
    [BepInPlugin("com.seninad.htftrainer", "HowToFish Trainer", "1.0.0")]
    public class Plugin : BaseUnityPlugin
    {
        public static bool GodMode = false;
        public static bool NoHunger = false;
        public static bool InfAmmo = false;
        public static float DmgMult = 1f;
        private bool showMenu = true;
        private int moneyToAdd = 10000;
        private Rect win = new Rect(20, 20, 420, 460);

        void Awake()
        {
            var h = new Harmony("com.seninad.htftrainer");
            h.PatchAll();
            Logger.LogInfo("HowToFish Trainer yuklendi. INSERT: menu");
        }

        void Update()
        {
            if (Input.GetKeyDown(KeyCode.Insert)) showMenu = !showMenu;
            if (Input.GetKeyDown(KeyCode.F1)) { GodMode = !GodMode; Logger.LogInfo("God: " + GodMode); }
            if (Input.GetKeyDown(KeyCode.F2)) { NoHunger = !NoHunger; Logger.LogInfo("Hunger: " + NoHunger); }
            if (Input.GetKeyDown(KeyCode.F4)) { InfAmmo = !InfAmmo; Logger.LogInfo("Ammo: " + InfAmmo); }
            if (Input.GetKeyDown(KeyCode.F5)) {
                DmgMult = DmgMult == 1f ? 2f : DmgMult == 2f ? 5f : DmgMult == 5f ? 10f : DmgMult == 10f ? 99999f : 1f;
            }
            if (Input.GetKeyDown(KeyCode.F6)) AddMoney(moneyToAdd);
            if (Input.GetKeyDown(KeyCode.F3)) {
                // Basit air jump: z Kiploda oyuncu rigidbody'ine yukari hiz ver
                var p = FindLocalPlayer();
                var rb = p != null ? p.GetComponent<Rigidbody>() : null;
                if (rb != null) rb.velocity = new Vector3(rb.velocity.x, 8f, rb.velocity.z);
            }
        }

        void OnGUI()
        {
            if (!showMenu) return;
            win = GUI.Window(1337, win, DrawWin, "How to Fish Trainer (BepInEx)");
        }

        void DrawWin(int id)
        {
            GodMode = GUILayout.Toggle(GodMode, "Olumsuzluk (F1)");
            NoHunger = GUILayout.Toggle(NoHunger, "Aclik kilidi (F2)");
            InfAmmo = GUILayout.Toggle(InfAmmo, "Sonsuz mermi (F4)");
            GUILayout.Label("Hasar carpi (F5): " + DmgMult);
            GUILayout.Label("Para (F6):");
            string s = GUILayout.TextField(moneyToAdd.ToString());
            if (int.TryParse(s, out int v)) moneyToAdd = v;
            if (GUILayout.Button("Para Ekle")) AddMoney(moneyToAdd);
            GUILayout.Label("F3: air jump | INSERT: menu");
            GUI.DragWindow();
        }

        GameObject FindLocalPlayer()
        {
            // FishNet: local oyuncu genelde "Player" tag'li objedir; tutmazsa dnSpy'dan sinif adini yaz
            var g = GameObject.FindWithTag("Player");
            return g != null ? g : Camera.main != null ? null : null;
        }

        void AddMoney(int amount)
        {
            // dnSpy'da Assembly-CSharp > MoneyManager static field adini bulup buraya yaz.
            // Asagisi reflection ile tum yuklu tiplerde "<Money>k__BackingField" / "_money" arar.
            foreach (var asm in System.AppDomain.CurrentDomain.GetAssemblies())
            {
                if (!asm.GetName().Name.Contains("Assembly-CSharp")) continue;
                foreach (var t in asm.GetTypes())
                {
                    foreach (var f in t.GetFields(System.Reflection.BindingFlags.Static |
                                                  System.Reflection.BindingFlags.Public |
                                                  System.Reflection.BindingFlags.NonPublic))
                    {
                        if (f.FieldType == typeof(int) &&
                            (f.Name.Contains("Money") || f.Name.Contains("_money")))
                        {
                            try
                            {
                                int cur = (int)f.GetValue(null);
                                f.SetValue(null, cur + amount);
                                Logger.LogInfo($"Para: {t.Name}.{f.Name} {cur} -> {cur + amount}");
                                return;
                            }
                            catch { }
                        }
                    }
                }
            }
            Logger.LogWarning("Para field bulunamadi, dnSpy ile bak.");
        }

        // ---- Harmony patchler: isimler dnSpy ile birebir tutmali ----
        [HarmonyPatch(typeof(object), "PLACEHOLDER")]
        static class Dummy { }

        // Ornek kalip (dnSpy'da gercek sinif adini bulunca typeof(object) yerine yaz):
        // [HarmonyPatch(typeof(PlayerVitals), "TakeDamage")]
        // static class NoDmg { static bool Prefix() => !GodMode; }
    }
}
