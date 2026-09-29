#include "mono_api.h"
#include <tlhelp32.h>
#include <cstring>

#define RESOLVE(name) p_##name = (decltype(p_##name))GetProcAddress(mono_, "mono_" #name)

bool MonoAPI::Attach() {
    if (ready_) return true;
    mono_ = GetModuleHandleW(L"mono-2.0-bdwgc.dll");
    if (!mono_) mono_ = GetModuleHandleW(L"mono.dll");
    if (!mono_) mono_ = LoadLibraryW(L"mono-2.0-bdwgc.dll");
    if (!mono_) return false;

    RESOLVE(get_root_domain);
    RESOLVE(thread_attach);
    RESOLVE(assembly_foreach);
    RESOLVE(assembly_get_image);
    RESOLVE(image_get_name);
    RESOLVE(class_from_name);
    RESOLVE(class_get_method_from_name);
    RESOLVE(compile_method);
    RESOLVE(jit_info_get_code_start);
    RESOLVE(jit_info_get_code_size);
    RESOLVE(jit_info_table_find);
    RESOLVE(class_vtable);
    RESOLVE(class_get_field_from_name);
    RESOLVE(field_static_get_value);
    RESOLVE(field_static_set_value);
    RESOLVE(method_get_name);
    RESOLVE(class_get_methods);
    RESOLVE(class_get_fields);
    RESOLVE(field_get_name);
    RESOLVE(class_get_parent);
    RESOLVE(class_get_name);
    RESOLVE(object_unbox);
    RESOLVE(method_signature);
    RESOLVE(signature_get_param_count);
    RESOLVE(signature_get_params);
    RESOLVE(type_get_type);
    RESOLVE(type_get_class);
    RESOLVE(field_get_type);
    RESOLVE(class_get_type);
    RESOLVE(type_get_object);
    RESOLVE(field_get_offset);
    RESOLVE(object_get_class);
    RESOLVE(runtime_invoke);
    RESOLVE(string_new);
    RESOLVE(string_to_utf8);
    RESOLVE(free);
    RESOLVE(array_length);
    RESOLVE(array_addr_with_size);
    if (!p_get_root_domain || !p_thread_attach || !p_class_from_name) return false;

    domain_ = p_get_root_domain();
    if (!domain_) return false;
    // Bu thread'i Mono'ya tanit (TLS kurulumu). Internal DLL'de sart.
    __try { p_thread_attach(domain_); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }

    ready_ = true;
    return true;
}

struct AsmCtx { MonoImage* img; const char* want; };
static void FindAsmCb(MonoAssembly* asm_, void* ud) {
    auto* ctx = (AsmCtx*)ud;
    if (ctx->img) return;
    auto& api = MonoAPI::Get();
    (void)api;
    // image ismini disaridan okuyacagiz; burada direkt dene:
    // Not: p_assembly_get_image MonoAPI icinde private, bu yuzden FindClass ayri dongu kurar.
    (void)asm_;
}

MonoClass* MonoAPI::FindClass(const char* namesp, const char* klass) {
    if (!ready_) return nullptr;
    struct Ctx { MonoAPI* api; const char* ns; const char* k; MonoClass* out = nullptr; };
    Ctx ctx{ this, namesp, klass };
    p_assembly_foreach([](MonoAssembly* a, void* ud) {
        auto* c = (Ctx*)ud;
        MonoImage* img = c->api->p_assembly_get_image(a);
        if (!img) return;
        const char* iname = c->api->p_image_get_name(img);
        if (!iname) return;
        std::string n = iname;
        // Sadece oyun kodu + corlib + fishnet tara
        if (n.find("Assembly-CSharp") == std::string::npos &&
            n.find("mscorlib") == std::string::npos &&
            n.find("FishNet") == std::string::npos &&
            n.find("UnityEngine") == std::string::npos) return;
        MonoClass* cl = c->api->p_class_from_name(img, c->ns ? c->ns : "", c->k);
        if (cl) c->out = cl;
    }, &ctx);
    return ctx.out;
}

MonoMethod* MonoAPI::FindMethod(MonoClass* klass, const char* name, int args) {
    if (!ready_ || !klass || !p_class_get_method_from_name) return nullptr;
    MonoMethod* m = nullptr;
    __try { m = p_class_get_method_from_name(klass, name, args); }
    __except (EXCEPTION_EXECUTE_HANDLER) { m = nullptr; }
    if (m) return m;
    // Overload sayisi tutmazsa (-1 her zaman calismaz), tum overloadlari dene
    if (args == -1) {
        for (int a = 0; a < 8; ++a) {
            __try { m = p_class_get_method_from_name(klass, name, a); }
            __except (EXCEPTION_EXECUTE_HANDLER) { m = nullptr; }
            if (m) return m;
        }
    }
    return nullptr;
}

MonoMethod* MonoAPI::FindMethodDeep(MonoClass* klass, const char* name, int args) {
    if (!ready_ || !klass || !name || !p_class_get_method_from_name) return nullptr;
    __try {
        for (MonoClass* c = klass; c; ) {
            MonoMethod* m = p_class_get_method_from_name(c, name, args);
            if (m) return m;
            if (args == -1) {
                for (int a = 0; a < 8; ++a) {
                    m = p_class_get_method_from_name(c, name, a);
                    if (m) return m;
                }
            }
            if (!p_class_get_parent) break;
            c = p_class_get_parent(c);
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// Isimle kaba bulma: buyuk/kucuk harf farkini tolere et (set_Ammo vs set_ammo)
MonoMethod* MonoAPI::FindMethodAny(MonoClass* klass, const char* name) {
    if (!ready_ || !klass || !name) return nullptr;
    MonoMethod* found = nullptr;
    ForEachMethod(klass, [&](MonoMethod* m) {
        if (found) return;
        const char* n = MethodName(m);
        if (!n) return;
        if (strcmp(n, name) == 0) { found = m; return; }
    });
    if (found) return found;
    ForEachMethod(klass, [&](MonoMethod* m) {
        if (found) return;
        const char* n = MethodName(m);
        if (!n) return;
        if (_stricmp(n, name) == 0) found = m;
    });
    return found;
}

void* MonoAPI::CompileMethod(MonoMethod* method, size_t* outSize) {
    if (!ready_ || !method || !p_compile_method) return nullptr;
    void* code = nullptr;
    __try { code = p_compile_method(method); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    if (!code) return nullptr;
    // Boyut cozumu opsiyonel: patlarsa kodu cope atma (eski bug buydu)
    if (outSize) {
        __try {
            if (p_jit_info_table_find && p_jit_info_get_code_size) {
                MonoJitInfo* ji = p_jit_info_table_find(domain_, (char*)code);
                if (ji) *outSize = (size_t)p_jit_info_get_code_size(ji);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { /* yoksay, code gecerli */ }
    }
    return code;
}

void* MonoAPI::VTable(MonoClass* klass) {
    if (!ready_ || !klass) return nullptr;
    __try { return p_class_vtable(domain_, klass); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool MonoAPI::IsInstanceOf(void* obj, MonoClass* klass) {
    if (!obj || !klass || !p_object_get_class) return false;
    __try { return p_object_get_class((MonoObject*)obj) == klass; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

const char* MonoAPI::ObjClassName(void* obj) {
    if (!obj || !p_object_get_class || !p_class_get_name) return "?";
    __try {
        MonoClass* c = p_object_get_class((MonoObject*)obj);
        if (!c) return "(null)";
        const char* n = p_class_get_name(c);
        return n ? n : "?";
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "(erisim ihlali)"; }
}

bool MonoAPI::GetStaticInt(MonoClass* klass, const char* fieldName, int& out) {
    if (!ready_ || !klass) return false;
    __try {
        MonoClassField* f = p_class_get_field_from_name(klass, fieldName);
        if (!f) return false;
        MonoVTable* vt = p_class_vtable(domain_, klass);
        if (!vt) return false;
        p_field_static_get_value(vt, f, &out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool MonoAPI::SetStaticInt(MonoClass* klass, const char* fieldName, int value) {
    if (!ready_ || !klass) return false;
    __try {
        MonoClassField* f = p_class_get_field_from_name(klass, fieldName);
        if (!f) return false;
        MonoVTable* vt = p_class_vtable(domain_, klass);
        if (!vt) return false;
        p_field_static_set_value(vt, f, &value);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool MonoAPI::GetStaticBool(MonoClass* klass, const char* fieldName, bool& out) {
    if (!ready_ || !klass) return false;
    __try {
        MonoClassField* f = FindFieldDeep(klass, fieldName);
        if (!f) return false;
        MonoVTable* vt = p_class_vtable(domain_, klass);
        if (!vt) return false;
        uint8_t v = 0;
        p_field_static_get_value(vt, f, &v);
        out = (v != 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool MonoAPI::SetStaticBool(MonoClass* klass, const char* fieldName, bool value) {
    if (!ready_ || !klass) return false;
    __try {
        MonoClassField* f = FindFieldDeep(klass, fieldName);
        if (!f) return false;
        MonoVTable* vt = p_class_vtable(domain_, klass);
        if (!vt) return false;
        uint8_t v = value ? 1 : 0;
        p_field_static_set_value(vt, f, &v);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* MonoAPI::GetStaticObject(MonoClass* klass, const char* fieldName) {
    if (!ready_ || !klass || !p_class_get_field_from_name) return nullptr;
    __try {
        MonoClassField* f = p_class_get_field_from_name(klass, fieldName);
        if (!f) return nullptr;
        MonoVTable* vt = p_class_vtable(domain_, klass);
        if (!vt) return nullptr;
        void* obj = nullptr;
        p_field_static_get_value(vt, f, &obj);
        return obj; // MonoObject* (nullptr olabilir)
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

MonoClassField* MonoAPI::FindFieldDeep(MonoClass* klass, const char* name) {
    if (!ready_ || !klass || !name) return nullptr;
    __try {
        for (MonoClass* c = klass; c; ) {
            if (p_class_get_field_from_name) {
                MonoClassField* f = p_class_get_field_from_name(c, name);
                if (f) return f;
            }
            // Yedek: numaralandir, isim karsilastir (bazi field'lar boyle bulunuyor)
            if (p_class_get_fields && p_field_get_name) {
                void* iter = nullptr;
                while (true) {
                    MonoClassField* f = (MonoClassField*)p_class_get_fields(c, &iter);
                    if (!f) break;
                    const char* n = p_field_get_name(f);
                    if (n && strcmp(n, name) == 0) return f;
                }
            }
            if (!p_class_get_parent) break;
            c = p_class_get_parent(c);
        }
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

MonoClassField* MonoAPI::GetField(MonoClass* klass, const char* fieldName) {
    return FindFieldDeep(klass, fieldName);
}

bool MonoAPI::FieldOffsetOf(MonoClassField* f, uint32_t& out) {
    if (!f || !p_field_get_offset) return false;
    __try { out = p_field_get_offset(f); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int MonoAPI::FieldTypeOf(MonoClassField* f) {
    if (!f || !p_field_get_type || !p_type_get_type) return -1;
    __try {
        MonoType* t = p_field_get_type(f);
        if (!t) return -1;
        return p_type_get_type(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

bool MonoAPI::GetFieldOffset(MonoClass* klass, const char* fieldName, uint32_t& out) {
    if (!ready_ || !klass || !p_field_get_offset) return false;
    __try {
        MonoClassField* f = FindFieldDeep(klass, fieldName);
        if (!f) return false;
        out = p_field_get_offset(f);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void* MonoAPI::Invoke(MonoMethod* m, void* obj, void** params) {
    if (!ready_ || !m || !p_runtime_invoke) return nullptr;
    __try {
        void* exc = nullptr;
        return p_runtime_invoke(m, obj, params, &exc);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool MonoAPI::InvokeChecked(MonoMethod* m, void* obj, void** params, void** result) {
    if (result) *result = nullptr;
    if (!ready_ || !m || !p_runtime_invoke) return false;
    __try {
        void* exc = nullptr;
        void* value = p_runtime_invoke(m, obj, params, &exc);
        if (result) *result = value;
        return exc == nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

MonoClass* MonoAPI::ObjectClass(void* obj) {
    if (!ready_ || !obj || !p_object_get_class) return nullptr;
    __try { return p_object_get_class((MonoObject*)obj); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void MonoAPI::ForEachMethod(MonoClass* klass, const std::function<void(MonoMethod*)>& cb) {
    if (!ready_ || !klass || !p_class_get_methods) return;
    void* iter = nullptr;
    while (true) {
        MonoMethod* m = nullptr;
        __try { m = (MonoMethod*)p_class_get_methods(klass, &iter); }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (!m) break;
        cb(m);
    }
}

void MonoAPI::ForEachField(MonoClass* klass, const std::function<void(MonoClassField*)>& cb) {
    if (!ready_ || !klass || !p_class_get_fields) return;
    void* iter = nullptr;
    while (true) {
        MonoClassField* f = nullptr;
        __try { f = (MonoClassField*)p_class_get_fields(klass, &iter); }
        __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (!f) break;
        cb(f);
    }
}

const char* MonoAPI::MethodName(MonoMethod* m) {
    if (!m || !p_method_get_name) return "?";
    const char* n = nullptr;
    __try { n = p_method_get_name(m); } __except (EXCEPTION_EXECUTE_HANDLER) { return "?"; }
    return n ? n : "?";
}

const char* MonoAPI::FieldName(MonoClassField* f) {
    if (!f || !p_field_get_name) return "?";
    const char* n = nullptr;
    __try { n = p_field_get_name(f); } __except (EXCEPTION_EXECUTE_HANDLER) { return "?"; }
    return n ? n : "?";
}

const char* MonoAPI::ClassName(MonoClass* c) {
    if (!c || !p_class_get_name) return "?";
    const char* n = nullptr;
    __try { n = p_class_get_name(c); } __except (EXCEPTION_EXECUTE_HANDLER) { return "?"; }
    return n ? n : "?";
}

int MonoAPI::UnboxInt(void* boxed) {
    if (!boxed || !p_object_unbox) return 0;
    __try { return *(int*)p_object_unbox((MonoObject*)boxed); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

float MonoAPI::UnboxFloat(void* boxed) {
    if (!boxed || !p_object_unbox) return 0.f;
    __try { return *(float*)p_object_unbox((MonoObject*)boxed); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0.f; }
}

void* MonoAPI::UnboxPtr(void* boxed) {
    if (!boxed || !p_object_unbox) return nullptr;
    __try { return p_object_unbox((MonoObject*)boxed); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

MonoClass* MonoAPI::GetParent(MonoClass* c) {
    if (!c || !p_class_get_parent) return nullptr;
    __try { return p_class_get_parent(c); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

MonoType* MonoAPI::ClassGetType(MonoClass* c) {
    if (!c || !p_class_get_type) return nullptr;
    __try { return p_class_get_type(c); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void* MonoAPI::TypeGetObject(MonoType* t) {
    if (!t || !p_type_get_object || !domain_) return nullptr;
    __try { return p_type_get_object(domain_, t); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

void* MonoAPI::NewString(const char* utf8) {
    if (!utf8 || !p_string_new || !domain_) return nullptr;
    __try { return p_string_new(domain_, utf8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

std::string MonoAPI::StringUtf8(void* monoString) {
    if (!monoString || !p_string_to_utf8) return {};
    char* s = p_string_to_utf8((MonoString*)monoString);
    if (!s) return {};
    std::string out(s);
    if (p_free) p_free(s);
    return out;
}

uintptr_t MonoAPI::ArrayLength(void* arr) {
    if (!arr || !p_array_length) return 0;
    __try { return p_array_length(arr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool MonoAPI::ArrayGetU8(void* arr, int index, uint8_t& out) {
    out = 0;
    if (!arr || !p_array_length || !p_array_addr_with_size || index < 0)
        return false;
    __try {
        uintptr_t n = p_array_length(arr);
        if ((uintptr_t)index >= n) return false;
        char* p = p_array_addr_with_size(arr, 1, (uintptr_t)index);
        if (!p) return false;
        out = *(uint8_t*)p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int MonoAPI::ParamCount(MonoMethod* m) {
    if (!m || !p_method_signature || !p_signature_get_param_count) return -1;
    __try {
        MonoMethodSignature* sig = p_method_signature(m);
        if (!sig) return -1;
        return (int)p_signature_get_param_count(sig);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

bool MonoAPI::ParamIsInt(MonoMethod* m, int index) {
    int code = ParamTypeCode(m, index);
    return code == 8 || code == 9; // MONO_TYPE_I4 / U4
}

int MonoAPI::ParamTypeCode(MonoMethod* m, int index) {
    if (!m || !p_method_signature || !p_signature_get_params || !p_type_get_type) return -1;
    __try {
        MonoMethodSignature* sig = p_method_signature(m);
        if (!sig) return -1;
        if ((int)p_signature_get_param_count(sig) <= index) return -1;
        void* iter = nullptr;
        MonoType* t = nullptr;
        for (int i = 0; i <= index; ++i) t = p_signature_get_params(sig, &iter);
        if (!t) return -1;
        return p_type_get_type(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

const char* MonoAPI::TypeCodeName(int code) {
    switch (code) {
        case 1: return "void"; case 2: return "bool"; case 3: return "char";
        case 4: return "i1"; case 5: return "u1"; case 6: return "i2"; case 7: return "u2";
        case 8: return "int"; case 9: return "uint"; case 10: return "i8"; case 11: return "u8";
        case 12: return "float"; case 13: return "double";         case 14: return "string"; case 16: return "generic";
        case 17: return "valuetype"; case 18: return "class"; case 20: return "szarray";
        case 27: return "ptr"; case 28: return "object"; case 30: return "mvar"; case 31: return "var";
        default: return "?";
    }
}

const char* MonoAPI::ParamClassName(MonoMethod* m, int index) {
    if (!m || !p_method_signature || !p_signature_get_params || !p_type_get_class || !p_class_get_name) return "?";
    __try {
        MonoMethodSignature* sig = p_method_signature(m);
        if (!sig) return "?";
        if ((int)p_signature_get_param_count(sig) <= index) return "?";
        void* iter = nullptr;
        MonoType* t = nullptr;
        for (int i = 0; i <= index; ++i) t = p_signature_get_params(sig, &iter);
        if (!t) return "?";
        MonoClass* c = p_type_get_class(t);
        if (!c) return "?";
        const char* n = p_class_get_name(c);
        return n ? n : "?";
    } __except (EXCEPTION_EXECUTE_HANDLER) { return "?"; }
}

int MonoAPI::FieldTypeCode(MonoClass* klass, const char* fieldName) {
    if (!klass || !fieldName || !p_field_get_type || !p_type_get_type) return -1;
    __try {
        MonoClassField* f = FindFieldDeep(klass, fieldName);
        if (!f) return -1;
        MonoType* t = p_field_get_type(f);
        if (!t) return -1;
        return p_type_get_type(t);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
