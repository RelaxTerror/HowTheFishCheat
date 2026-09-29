#pragma once
// Unity Mono (mono-2.0-bdwgc.dll) dinamik baglanti.
// dnSpy / BepInEx ile bulunan Assembly-CSharp siniflarini runtime'da cozer.
#include <windows.h>
#include <string>
#include <functional>

struct MonoDomain;
struct MonoAssembly;
struct MonoImage;
struct MonoClass;
struct MonoMethod;
struct MonoVTable;
struct MonoClassField;
struct MonoJitInfo;
struct MonoMethodDesc;
struct MonoObject;
struct MonoProperty;
struct MonoMethodSignature;
struct MonoType;
struct MonoString;

class MonoAPI {
public:
    static MonoAPI& Get() { static MonoAPI i; return i; }

    bool Attach(); // mono dll'i bul + thread'i domain'e bagla
    bool IsReady() const { return ready_; }

    // Assembly-CSharp icindeki sinifi bul (namespace bos olabilir)
    MonoClass* FindClass(const char* namesp, const char* klass);
    // Siniftaki metodu bul
    MonoMethod* FindMethod(MonoClass* klass, const char* name, int args = -1);
    // Base siniflara tirmanan metod aramasi (ornegin Cod objesinde Creature metodlari).
    MonoMethod* FindMethodDeep(MonoClass* klass, const char* name, int args = -1);
    // Fallback: overload sayisina bakmadan isimle bul (case-sensitive, sonra case-insensitive)
    MonoMethod* FindMethodAny(MonoClass* klass, const char* name);
    // Metodu JIT'le ve native kod adresini dondur (patch icin)
    void* CompileMethod(MonoMethod* method, size_t* outSize = nullptr);

    // Field pointer'indan dogrudan offset/tip (isim aramasiz).
    bool FieldOffsetOf(MonoClassField* f, uint32_t& out);
    int FieldTypeOf(MonoClassField* f);
    // Field bul: isim aramasi + numaralandirma taramasi + base tirmanma.
    MonoClassField* FindFieldDeep(MonoClass* klass, const char* name);
    bool GetStaticInt(MonoClass* klass, const char* fieldName, int& out);
    bool SetStaticInt(MonoClass* klass, const char* fieldName, int value);
    // Static bool oku/yaz (ClientSettings.CheatsEnabled gibi). 1 byte, guvenli.
    bool GetStaticBool(MonoClass* klass, const char* fieldName, bool& out);
    bool SetStaticBool(MonoClass* klass, const char* fieldName, bool value);
    // Static object field (Player.LocalPlayer, PlayerUI._instance gibi)
    void* GetStaticObject(MonoClass* klass, const char* fieldName);

    // Instance field offset + oku/yaz (Player._playerVitals -> Vitals.health gibi)
    bool GetFieldOffset(MonoClass* klass, const char* fieldName, uint32_t& out);
    MonoClassField* GetField(MonoClass* klass, const char* fieldName);

    // Siniftaki tum metodlari loglamak icin (offset guncelleme asamasinda ise yarar)
    void ForEachMethod(MonoClass* klass, const std::function<void(MonoMethod*)>& cb);
    void ForEachField(MonoClass* klass, const std::function<void(MonoClassField*)>& cb);

    const char* MethodName(MonoMethod* m);
    const char* FieldName(MonoClassField* f);
    const char* ClassName(MonoClass* c);
    int UnboxInt(void* boxed);
    float UnboxFloat(void* boxed);
    // Metodun parametre sayisi (overload ayiklamak icin). Hata -> -1.
    int ParamCount(MonoMethod* m);
    // Parametre tipi int/uint mi? (AddMoney(int) dogrulamasi)
    bool ParamIsInt(MonoMethod* m, int index);
    // Parametre tip kodu (MONO_TYPE: 2=bool 8=i4 14=string 18=class ...). Hata -> -1.
    int ParamTypeCode(MonoMethod* m, int index);
    const char* TypeCodeName(int code);
    // Field'in tip kodu (8=i4 9=u4 12=float ...). Hata -> -1.
    int FieldTypeCode(MonoClass* klass, const char* fieldName);
    // Referans-tip parametrenin sinif adi (AddMoney 2. param turu icin). Hata -> "?".
    const char* ParamClassName(MonoMethod* m, int index);
    void* VTable(MonoClass* klass);

    // Obje gercekten bu sinifin ornegi mi? (bayat/olu pointer yakalar)
    bool IsInstanceOf(void* obj, MonoClass* klass);
    // Objenin gercek sinif adi (tani icin). Hata -> "?" / "(null)".
    const char* ObjClassName(void* obj);
    // Yeni: generic-disi yardimcilar (silent aim icin)
    MonoDomain* Domain() const { return domain_; }
    MonoClass* GetParent(MonoClass* c);
    void* UnboxPtr(void* boxed);
    MonoType* ClassGetType(MonoClass* c);
    void* TypeGetObject(MonoType* t);
    void* NewString(const char* utf8);
    std::string StringUtf8(void* monoString);
    // Mono byte[] okuma (SlotMachineManager.SendRoll itemIDs/itemSkins gibi).
    uintptr_t ArrayLength(void* arr);
    bool ArrayGetU8(void* arr, int index, uint8_t& out);

    // Obje uzerinde metod cagir (ornegin PlayerUI.SetMoney)
    void* Invoke(MonoMethod* m, void* obj, void** params);
    // Managed exception olusursa false doner; void metodlarin basarisini ayirt eder.
    bool InvokeChecked(MonoMethod* m, void* obj, void** params, void** result = nullptr);
    MonoClass* ObjectClass(void* obj);

private:
    MonoAPI() = default;
    bool ready_ = false;
    HMODULE mono_ = nullptr;
    MonoDomain* domain_ = nullptr;
    MonoImage* corlib_ = nullptr;

    // --- mono exportlari (GetProcAddress ile) ---
    MonoDomain*  (__cdecl *p_get_root_domain)() = nullptr;
    MonoDomain*  (__cdecl *p_thread_attach)(MonoDomain*) = nullptr;
    void         (__cdecl *p_assembly_foreach)(void(*)(MonoAssembly*, void*), void*) = nullptr;
    MonoImage*   (__cdecl *p_assembly_get_image)(MonoAssembly*) = nullptr;
    const char*  (__cdecl *p_image_get_name)(MonoImage*) = nullptr;
    MonoClass*   (__cdecl *p_class_from_name)(MonoImage*, const char*, const char*) = nullptr;
    MonoMethod*  (__cdecl *p_class_get_method_from_name)(MonoClass*, const char*, int) = nullptr;
    void*        (__cdecl *p_compile_method)(MonoMethod*) = nullptr;
    void*        (__cdecl *p_jit_info_get_code_start)(MonoJitInfo*) = nullptr;
    int          (__cdecl *p_jit_info_get_code_size)(MonoJitInfo*) = nullptr;
    MonoJitInfo* (__cdecl *p_jit_info_table_find)(MonoDomain*, char*) = nullptr;
    MonoVTable*  (__cdecl *p_class_vtable)(MonoDomain*, MonoClass*) = nullptr;
    MonoClassField* (__cdecl *p_class_get_field_from_name)(MonoClass*, const char*) = nullptr;
    void         (__cdecl *p_field_static_get_value)(MonoVTable*, MonoClassField*, void*) = nullptr;
    void         (__cdecl *p_field_static_set_value)(MonoVTable*, MonoClassField*, void*) = nullptr;
    const char*  (__cdecl *p_method_get_name)(MonoMethod*) = nullptr;
    void*        (__cdecl *p_class_get_methods)(MonoClass*, void**) = nullptr;
    MonoClassField* (__cdecl *p_class_get_fields)(MonoClass*, void**) = nullptr;
    const char*  (__cdecl *p_field_get_name)(MonoClassField*) = nullptr;
    const char*  (__cdecl *p_class_get_name)(MonoClass*) = nullptr;
    MonoClass*   (__cdecl *p_class_get_parent)(MonoClass*) = nullptr;
    void*        (__cdecl *p_object_unbox)(MonoObject*) = nullptr;
    MonoMethodSignature* (__cdecl *p_method_signature)(MonoMethod*) = nullptr;
    unsigned int (__cdecl *p_signature_get_param_count)(MonoMethodSignature*) = nullptr;
    MonoType*    (__cdecl *p_signature_get_params)(MonoMethodSignature*, void**) = nullptr;
    int          (__cdecl *p_type_get_type)(MonoType*) = nullptr;
    MonoClass*   (__cdecl *p_type_get_class)(MonoType*) = nullptr;
    MonoType*    (__cdecl *p_field_get_type)(MonoClassField*) = nullptr;
    MonoType*    (__cdecl *p_class_get_type)(MonoClass*) = nullptr;
    void*        (__cdecl *p_type_get_object)(MonoDomain*, MonoType*) = nullptr;
    uint32_t     (__cdecl *p_field_get_offset)(MonoClassField*) = nullptr;
    MonoClass*   (__cdecl *p_object_get_class)(MonoObject*) = nullptr;
    void*        (__cdecl *p_runtime_invoke)(MonoMethod*, void*, void**, void**) = nullptr;
    MonoString*  (__cdecl *p_string_new)(MonoDomain*, const char*) = nullptr;
    char*        (__cdecl *p_string_to_utf8)(MonoString*) = nullptr;
    void         (__cdecl *p_free)(void*) = nullptr;
    uintptr_t    (__cdecl *p_array_length)(void*) = nullptr;
    char*        (__cdecl *p_array_addr_with_size)(void*, int, uintptr_t) = nullptr;
};
