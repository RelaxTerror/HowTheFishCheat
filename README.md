# How to Fish — Internal Trainer (Türkçe)

Steam: **How to Fish** (App 4001890, Dazed Games) için internal trainer iskeleti.
**C++ DLL + ImGui (DX11) + Unity Mono JIT patch** kullanır.

> Bu repo eğitim amaçlıdır. Online / public lobide kullanma — ban yersin.
> Singleplayer veya arkadaşlarınla açtığın özel lobide dene.

---

## 1. Nasıl çalışıyor?

Oyun **Unity Mono** ( `mono-2.0-bdwgc.dll` + `Assembly-CSharp.dll` ).

Trainer, oyunun içine DLL olarak enjekte olur ve:

1. **DX11 Present** fonksiyonunu hooklayıp **ImGui menü** çizer (`INSERT` ile aç/kapa).
2. Mono runtime'a bağlanıp `Assembly-CSharp` içindeki sınıfları bulur:
   - `PlayerVitals.TakeDamage / LocalHit / DamageFromFullness / ApplyNewFire / ApplyNewPoison` → **F1 ölümsüzlük**
   - `PlayerVitals.LowerFullness / LowerFullnessTick` → **F2 açlık kilidi**
   - `Weapon.set_Ammo` → **F4 sınırsız mermi**
   - `MoneyManager.<Money>k__BackingField` → **F6 para ekle**
3. JIT'lenmiş fonksiyonun ilk byte'ına `RET (0xC3)` yazar → fonksiyon çalışmaz olur.
   Kapatınca orijinal byte geri yazılır (temiz çıkış).

Sınıf/metod isimleri açık trainer araştırmasından + dnSpy kalıbından gelir.
Oyun güncellenirse (şu an ~1.0.11) adresler değişir ama trainer her açılışta
JIT adresini **yeniden çözdüğü** için genelde kırılmaz. İsim değişirse
`src/cheats.cpp` içindeki listeyi güncelle.

---

## 2. Gereksinimler

- Windows 10/11 **64-bit**
- **Visual Studio 2022** (Desktop development with C++ + Windows SDK)
- **CMake 3.20+** (sende 4.3.2 var ✔)
- Steam'de oyun kurulu

ImGui + MinHook otomatik iner (`FetchContent`, internet gerekli).

---

## 3. Derleme

```powershell
# Bu klasorde:
cmake -S . -B build -A x64 -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Çıktılar:

- `build/Release/HowToFishInternal_v80.dll` (oyuna enjekte edilecek)
- `build/Release/Injector.exe`

Derleme hatası alırsan:

- `-A x64` yazdığından emin ol (32-bit olmaz).
- VS'de "Desktop development with C++" kurulu olsun.

---

## 4. Kullanım

1. Oyunu başlat (ana menüye kadar gel).
2. Admin bir PowerShell aç:
   ```powershell
   .\build\Release\Injector.exe "How to Fish.exe" .\build\Release\HowToFishInternal_v80.dll
   ```
   > EXE adı farklıysa Görev Yöneticisi'nden bak. PID ile de olur:
   > `Injector.exe 12345 HowToFishInternal_v80.dll`
3. Oyuna dön → **INSERT** veya **HOME** ile menü.
4. Tuşlar:

| Tuş | İşlev |
|-----|-------|
| INSERT / HOME | Menü aç/kapa |
| F1 | Ölümsüzlük |
| F2 | Açlık kilidi |
| F3 | Air-jump bayrağı |
| F4 | Sınırsız mermi |
| F5 | Hasar çarpanı (1x→2x→5x→10x→One-Shot) |
| F6 | Para ekle (+10.000) |
| END | Temiz çıkış (patchleri geri alır + eject) |

Menüden de hepsi tıklanabilir + log penceresi var.

---

## 5. Çalışmazsa — offset güncelleme (5 dk)

1. **dnSpy** indir, `How to Fish_Data/Managed/Assembly-CSharp.dll` dosyasını aç.
2. Solda ara: `PlayerVitals`, `Weapon`, `MoneyManager`, `PlayerUI`.
3. Metod adları değişmiş mi bak:
   - `TakeDamage`, `LocalHit`, `LowerFullness`, `set_Ammo`, `SetMoney`
4. Değişmişse `src/cheats.cpp` içindeki `kGodTargets / kHungerTargets / kAmmoTargets` listesini düzelt, tekrar derle.
5. Para field adı için: `MoneyManager` sınıfı → sağda Fields → static int olan (genelde `<Money>k__BackingField` veya `_money`). Adını `cheats.cpp :: AddMoney` içindeki `fields[]` dizisine ekle.

Alternatif hızlı yol: **Cheat Engine** ile para adresini bul → `memory.h :: ReadChain` ile pointer chain yaz.

---

## 6. Kolay yol: BepInEx (tavsiye)

C++ inject ile uğraşmak istemiyorsan `BepInEx/Plugin.cs` var.
BepInEx 5, Unity Mono oyunlarda inject'siz **internal mod** gibi çalışır,
DX hook gerekmez, daha stabil.

1. [BepInEx 5 x64](https://github.com/BepInEx/BepInEx/releases) indir.
2. Oyunun klasörüne (`How to Fish.exe`nin yanına) çıkar, oyunu 1 kez aç-kapat.
3. Derle:
   ```powershell
   $env:HOWTOFISH_DIR="C:\Program Files (x86)\Steam\steamapps\common\How to Fish"
   dotnet build BepInEx\HowToFishTrainer.csproj -c Release
   ```
4. Çıkan `HowToFishTrainer.dll` dosyasını `BepInEx/plugins/` içine at.
5. Oyunu aç → `INSERT` menü.

> `Plugin.cs` içindeki Harmony patch isimlerini dnSpy'dan doğrulayıp
> `typeof(object)` placeholder'larını gerçek sınıflarla değiştir
> (örn. `typeof(PlayerVitals)`). Kalıp dosyada yorum olarak var.

---

## 7. Dosya yapısı

```
CMakeLists.txt          -> ImGui+MinHook otomatik indirir, DLL+Injector derler
src/
  dllmain.cpp           -> DllMain, hotkey dongusu, temiz cikis
  renderer.h/.cpp       -> DX11 Present hook + WndProc + ImGui init
  gui.h/.cpp            -> ImGui menusu (Turkce)
  mono_api.h/.cpp       -> mono-2.0-bdwgc.dll dinamik baglanti
  cheats.h/.cpp         -> F1-F6 hile mantigi (JIT RET patch)
  memory.h              -> Patch/Nop/PatternScan/ReadChain
injector/injector.cpp   -> LoadLibrary injector
BepInEx/Plugin.cs       -> Inject'siz alternatif (tavsiye edilen stabil yol)
```

---

## 8. SSS

**Inject ettim, menü gelmiyor?**
- Oyunun DX11 ile açıldığından emin ol (Unity `-force-d3d11` parametresi dene).
- DLL + oyun ikisi de 64-bit olmalı.
- Menü tuşu INSERT. Log için önce enjekte et, sonra oyuna tıkla.

**F1 bastım, logda "Sınıf bulunamadı" yazıyor?**
- Oyun güncellenmiş, sınıf adı değişmiş. Bölüm 5'teki dnSpy adımını yap.

**Online'da kullanabilir miyim?**
- Hayır. FishNet ile para/canı server da tutuyor olabilir, desync/ban olur.
Singleplayer'da dene.

**VAC ban var mı?**
- How to Fish'te VAC yok (bildiğimiz kadarıyla), ama yine de ana hesabında
public lobide hile açma.
