#include "jni/jni.h"

#include "core/log.h"
#include "runtime.h"
#include "shims/common.h"

#include <algorithm>
#include <array>
#include <set>

namespace rfu::jni {

namespace {

constexpr std::uint64_t kJniOk = 0;
constexpr std::uint64_t kJniVersion16 = 0x00010006;

// --- the JNIEnv function table, by index ------------------------------------------------------

const std::array<const char*, 10> kTypes{"Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double", "Void"};
const std::array<const char*, 8> kPrims{"Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double"};
constexpr char kPrimCodes[] = "ZBCSIJFD";

std::vector<std::string> env_function_names() {
    std::vector<std::string> n{"reserved0", "reserved1", "reserved2", "reserved3", "GetVersion", "DefineClass", "FindClass",
                               "FromReflectedMethod", "FromReflectedField", "ToReflectedMethod", "GetSuperclass",
                               "IsAssignableFrom", "ToReflectedField", "Throw", "ThrowNew", "ExceptionOccurred",
                               "ExceptionDescribe", "ExceptionClear", "FatalError", "PushLocalFrame", "PopLocalFrame",
                               "NewGlobalRef", "DeleteGlobalRef", "DeleteLocalRef", "IsSameObject", "NewLocalRef",
                               "EnsureLocalCapacity", "AllocObject", "NewObject", "NewObjectV", "NewObjectA", "GetObjectClass",
                               "IsInstanceOf", "GetMethodID"};
    auto calls = [&](const std::string& prefix) {
        for (const char* t : kTypes)
            for (const char* suffix : {"", "V", "A"}) n.push_back(prefix + t + "Method" + suffix);
    };
    calls("Call");
    calls("CallNonvirtual");
    n.push_back("GetFieldID");
    for (int i = 0; i < 9; ++i) n.push_back(std::string("Get") + kTypes[i] + "Field");
    for (int i = 0; i < 9; ++i) n.push_back(std::string("Set") + kTypes[i] + "Field");
    n.push_back("GetStaticMethodID");
    calls("CallStatic");
    n.push_back("GetStaticFieldID");
    for (int i = 0; i < 9; ++i) n.push_back(std::string("GetStatic") + kTypes[i] + "Field");
    for (int i = 0; i < 9; ++i) n.push_back(std::string("SetStatic") + kTypes[i] + "Field");
    for (const char* s : {"NewString", "GetStringLength", "GetStringChars", "ReleaseStringChars", "NewStringUTF",
                          "GetStringUTFLength", "GetStringUTFChars", "ReleaseStringUTFChars", "GetArrayLength",
                          "NewObjectArray", "GetObjectArrayElement", "SetObjectArrayElement"})
        n.push_back(s);
    for (const char* pattern : {"New%sArray", "Get%sArrayElements", "Release%sArrayElements", "Get%sArrayRegion", "Set%sArrayRegion"})
        for (const char* p : kPrims) {
            std::string s = pattern;
            s.replace(s.find("%s"), 2, p);
            n.push_back(s);
        }
    for (const char* s : {"RegisterNatives", "UnregisterNatives", "MonitorEnter", "MonitorExit", "GetJavaVM", "GetStringRegion",
                          "GetStringUTFRegion", "GetPrimitiveArrayCritical", "ReleasePrimitiveArrayCritical",
                          "GetStringCritical", "ReleaseStringCritical", "NewWeakGlobalRef", "DeleteWeakGlobalRef",
                          "ExceptionCheck", "NewDirectByteBuffer", "GetDirectBufferAddress", "GetDirectBufferCapacity",
                          "GetObjectRefType"})
        n.push_back(s);
    return n;
}

std::uint64_t element_size(char e) {
    switch (e) {
    case 'Z':
    case 'B': return 1;
    case 'C':
    case 'S': return 2;
    case 'I':
    case 'F': return 4;
    default: return 8;
    }
}

// Which primitive a "New<Prim>Array"-style function name is about.
char prim_of(std::string_view name) {
    for (int i = 0; i < 8; ++i)
        if (name.find(kPrims[i]) != std::string_view::npos) return kPrimCodes[i];
    return 'B';
}

std::string return_type(const std::string& signature) {
    auto close = signature.find(')');
    return close == std::string::npos ? signature : signature.substr(close + 1);
}

Jni& J(CallContext& c) { return get(c.rt()); }

void ret_value(CallContext& c, const std::string& type, std::uint64_t v) {
    if (type == "F") c.cpu().jit().SetVector(0, {v & 0xFFFFFFFF, 0});
    else if (type == "D") c.cpu().jit().SetVector(0, {v, 0});
    c.ret(v);
}

std::string describe(const Member& m) {
    return fmt::format("{}{}.{}{}", m.is_static ? "static " : "", m.class_name, m.name, m.signature);
}

void log_once(Jni& j, GuestAddr id, const Member& m, const char* what) {
    static std::mutex mutex;
    static std::set<std::pair<Jni*, GuestAddr>> seen;
    std::lock_guard lock(mutex);
    if (seen.insert({&j, id}).second) log::info("java: {} {} isn't implemented; using a default", what, describe(m));
}

// --- shims ---------------------------------------------------------------------------------------

void unimplemented(CallContext& c) {
    if (c.thunk().calls.load() == 1) log::warn("unimplemented JNI function {} called from {:#x}", c.thunk().name, c.x(30));
    c.ret(0);
}

void call_method(CallContext& c) {
    auto m = J(c).member(c.x(2));
    if (!m) {
        log::warn("{} with an unknown method id {:#x}", c.thunk().name, c.x(2));
        return c.ret(0);
    }
    std::string type = return_type(m->signature);
    if (auto impl = J(c).implementation(*m)) return ret_value(c, type, (*impl)(c, *m));
    log_once(J(c), c.x(2), *m, "method");
    ret_value(c, type, J(c).default_value(m->signature));
}

void get_field(CallContext& c) {
    auto m = J(c).member(c.x(2));
    if (!m) return c.ret(0);
    if (auto impl = J(c).implementation(*m)) return ret_value(c, m->signature, (*impl)(c, *m));
    log_once(J(c), c.x(2), *m, "field");
    ret_value(c, m->signature, J(c).default_value(")" + m->signature));
}

void get_member_id(CallContext& c) {
    auto cls = J(c).object(c.x(1));
    std::string name = c.str(2), sig = c.str(3);
    std::string_view fn = c.thunk().name;
    bool is_static = fn.find("Static") != std::string_view::npos;
    bool is_field = fn.find("Field") != std::string_view::npos;
    c.ret(J(c).member_id({cls ? cls->class_name : "?", name, sig, is_static, is_field}));
}

void new_object(CallContext& c) {
    auto cls = J(c).object(c.x(1));
    std::string name = cls ? cls->class_name : "java/lang/Object";
    if (auto m = J(c).member(c.x(2)); m && c.thunk().name != "AllocObject") {
        if (auto impl = J(c).implementation(*m)) return c.ret((*impl)(c, *m));
        log_once(J(c), c.x(2), *m, "constructor");
    }
    c.ret(J(c).new_instance(name));
}

void new_prim_array(CallContext& c) { c.ret(J(c).new_array(prim_of(c.thunk().name.substr(3)), c.x(1))); }

void array_elements(CallContext& c) {
    auto a = J(c).object(c.x(1));
    if (c.x(2)) c.mem().write<std::uint8_t>(c.x(2), 0);  // isCopy = false: it's the real storage
    c.ret(a ? a->data : 0);
}

void array_region(CallContext& c) {
    // Get/Set<Prim>ArrayRegion(env, array, start, len, buf)
    auto a = J(c).object(c.x(1));
    if (!a || !a->data) return;
    std::uint64_t size = element_size(a->element);
    GuestAddr elems = a->data + c.x(2) * size, buf = c.x(4);
    std::uint64_t bytes = c.x(3) * size;
    if (c.thunk().name[0] == 'G') std::memmove(c.mem().host(buf), c.mem().host(elems), bytes);
    else std::memmove(c.mem().host(elems), c.mem().host(buf), bytes);
}

std::u16string to_utf16(const std::string& utf8) {
    std::u16string out;
    for (char32_t ch : utf8_to_u32(utf8)) {
        if (ch >= 0x10000) {
            ch -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (ch >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (ch & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(ch));
        }
    }
    return out;
}

std::string from_utf16(const char16_t* s, std::size_t n) {
    std::u32string w;
    for (std::size_t i = 0; i < n; ++i) {
        char32_t ch = s[i];
        if (ch >= 0xD800 && ch < 0xDC00 && i + 1 < n) ch = 0x10000 + ((ch - 0xD800) << 10) + (s[++i] - 0xDC00);
        w.push_back(ch);
    }
    return u32_to_utf8(w);
}

std::string string_of(CallContext& c, GuestAddr s) {
    auto o = J(c).object(s);
    return o ? o->text : std::string();
}

void register_env(std::unordered_map<std::string, ShimFn>& f) {
    f["GetVersion"] = [](CallContext& c) { c.ret(kJniVersion16); };
    f["FindClass"] = [](CallContext& c) {
        std::string name = c.str(1);
        std::replace(name.begin(), name.end(), '.', '/');
        c.ret(J(c).find_class(name));
    };
    f["GetSuperclass"] = [](CallContext& c) { c.ret(J(c).find_class("java/lang/Object")); };
    f["IsAssignableFrom"] = [](CallContext& c) { c.ret(1); };
    f["IsInstanceOf"] = [](CallContext& c) {
        auto o = J(c).object(c.x(1));
        auto cls = J(c).object(c.x(2));
        c.ret(o && cls && (o->class_name == cls->class_name || cls->class_name == "java/lang/Object") ? 1 : 0);
    };
    f["Throw"] = [](CallContext& c) {
        log::warn("native code threw a Java exception");
        c.ret(0);
    };
    f["ThrowNew"] = [](CallContext& c) {
        auto cls = J(c).object(c.x(1));
        log::warn("native code threw {}: {}", cls ? cls->class_name : "?", c.x(2) ? c.str(2) : "");
        c.ret(0);
    };
    for (const char* name : {"ExceptionOccurred", "ExceptionCheck", "MonitorEnter", "MonitorExit", "EnsureLocalCapacity",
                             "PushLocalFrame", "UnregisterNatives"})
        f[name] = [](CallContext& c) { c.ret(0); };
    for (const char* name : {"ExceptionDescribe", "ExceptionClear", "DeleteGlobalRef", "DeleteLocalRef", "DeleteWeakGlobalRef",
                             "ReleasePrimitiveArrayCritical"})
        f[name] = [](CallContext& c) {};
    f["FatalError"] = [](CallContext& c) { throw GuestFault(std::string("JNI FatalError: ") + (c.x(1) ? c.str(1) : "")); };
    for (const char* name : {"PopLocalFrame", "NewGlobalRef", "NewLocalRef", "NewWeakGlobalRef"})
        f[name] = [](CallContext& c) { c.ret(c.x(1)); };
    f["IsSameObject"] = [](CallContext& c) { c.ret(c.x(1) == c.x(2) ? 1 : 0); };
    f["GetObjectRefType"] = [](CallContext& c) { c.ret(2); };  // JNIGlobalRefType
    f["GetObjectClass"] = [](CallContext& c) {
        auto o = J(c).object(c.x(1));
        std::string name = !o ? "java/lang/Object" : o->kind == Object::Kind::Class ? "java/lang/Class" : o->class_name;
        c.ret(J(c).find_class(name));
    };
    for (const char* name : {"AllocObject", "NewObject", "NewObjectV", "NewObjectA"}) f[name] = new_object;
    for (const char* name : {"GetMethodID", "GetStaticMethodID", "GetFieldID", "GetStaticFieldID"}) f[name] = get_member_id;

    // Strings.
    f["NewStringUTF"] = [](CallContext& c) { c.ret(c.x(1) ? J(c).new_string(c.str(1)) : 0); };
    f["NewString"] = [](CallContext& c) { c.ret(J(c).new_string(from_utf16(c.ptr<char16_t>(1), c.x(2)))); };
    f["GetStringUTFLength"] = [](CallContext& c) { c.ret(string_of(c, c.x(1)).size()); };
    f["GetStringLength"] = [](CallContext& c) { c.ret(to_utf16(string_of(c, c.x(1))).size()); };
    f["GetStringUTFChars"] = [](CallContext& c) {
        if (c.x(2)) c.mem().write<std::uint8_t>(c.x(2), 1);
        c.ret(c.rt().heap().strdup(string_of(c, c.x(1))));
    };
    f["ReleaseStringUTFChars"] = [](CallContext& c) { c.rt().heap().free(c.x(2)); };
    auto get_chars = [](CallContext& c) {
        std::u16string s = to_utf16(string_of(c, c.x(1)));
        GuestAddr buf = c.rt().heap().alloc(2 * (s.size() + 1));
        c.mem().write_bytes(buf, s.c_str(), 2 * (s.size() + 1));
        if (c.x(2)) c.mem().write<std::uint8_t>(c.x(2), 1);
        c.ret(buf);
    };
    f["GetStringChars"] = get_chars;
    f["GetStringCritical"] = get_chars;
    f["ReleaseStringChars"] = [](CallContext& c) { c.rt().heap().free(c.x(2)); };
    f["ReleaseStringCritical"] = [](CallContext& c) { c.rt().heap().free(c.x(2)); };
    f["GetStringRegion"] = [](CallContext& c) {
        std::u16string s = to_utf16(string_of(c, c.x(1)));
        std::uint64_t start = c.x(2), len = c.x(3);
        if (start + len <= s.size()) c.mem().write_bytes(c.x(4), s.data() + start, 2 * len);
    };
    f["GetStringUTFRegion"] = [](CallContext& c) {
        // start and len count UTF-16 units; good enough for the ASCII strings this sees.
        std::string s = string_of(c, c.x(1));
        std::uint64_t start = std::min<std::uint64_t>(c.x(2), s.size()), len = std::min<std::uint64_t>(c.x(3), s.size() - start);
        c.mem().write_bytes(c.x(4), s.data() + start, len);
        c.mem().write<std::uint8_t>(c.x(4) + len, 0);
    };

    // Arrays.
    f["GetArrayLength"] = [](CallContext& c) {
        auto a = J(c).object(c.x(1));
        c.ret(a ? a->length : 0);
    };
    f["NewObjectArray"] = [](CallContext& c) {
        auto cls = J(c).object(c.x(2));
        GuestAddr a = J(c).new_array('L', c.x(1), cls ? cls->class_name : "java/lang/Object");
        if (c.x(3))
            for (std::uint64_t i = 0; i < c.x(1); ++i) J(c).set_element(a, i, c.x(3));
        c.ret(a);
    };
    f["GetObjectArrayElement"] = [](CallContext& c) {
        auto a = J(c).object(c.x(1));
        c.ret(a && c.x(2) < a->elements.size() ? a->elements[c.x(2)] : 0);
    };
    f["SetObjectArrayElement"] = [](CallContext& c) { J(c).set_element(c.x(1), c.x(2), c.x(3)); };
    for (const char* p : kPrims) {
        f[std::string("New") + p + "Array"] = new_prim_array;
        f[std::string("Get") + p + "ArrayElements"] = array_elements;
        f[std::string("Release") + p + "ArrayElements"] = [](CallContext& c) {};
        f[std::string("Get") + p + "ArrayRegion"] = array_region;
        f[std::string("Set") + p + "ArrayRegion"] = array_region;
    }
    f["GetPrimitiveArrayCritical"] = array_elements;
    f["NewDirectByteBuffer"] = [](CallContext& c) { c.ret(J(c).new_byte_buffer(c.x(1), c.x(2))); };
    f["GetDirectBufferAddress"] = [](CallContext& c) {
        auto o = J(c).object(c.x(1));
        c.ret(o ? o->data : 0);
    };
    f["GetDirectBufferCapacity"] = [](CallContext& c) {
        auto o = J(c).object(c.x(1));
        c.ret(o ? o->length : ~0ull);
    };

    // Natives and the VM.
    f["RegisterNatives"] = [](CallContext& c) {
        auto cls = J(c).object(c.x(1));
        std::vector<NativeMethod> methods;
        for (std::uint64_t i = 0; i < c.x(3); ++i) {
            GuestAddr m = c.x(2) + 24 * i;  // JNINativeMethod { name, signature, fnPtr }
            methods.push_back({c.mem().read_cstr(c.mem().read<GuestAddr>(m)), c.mem().read_cstr(c.mem().read<GuestAddr>(m + 8)),
                               c.mem().read<GuestAddr>(m + 16)});
        }
        J(c).register_natives(cls ? cls->class_name : "?", std::move(methods));
        c.ret(kJniOk);
    };
    f["GetJavaVM"] = [](CallContext& c) {
        c.mem().write<GuestAddr>(c.x(1), J(c).vm());
        c.ret(kJniOk);
    };

    // Method calls and fields: every type and variant shares one implementation.
    for (const char* t : kTypes)
        for (const char* prefix : {"Call", "CallNonvirtual", "CallStatic"})
            for (const char* suffix : {"", "V", "A"}) f[std::string(prefix) + t + "Method" + suffix] = call_method;
    for (int i = 0; i < 9; ++i) {
        f[std::string("Get") + kTypes[i] + "Field"] = get_field;
        f[std::string("GetStatic") + kTypes[i] + "Field"] = get_field;
        f[std::string("Set") + kTypes[i] + "Field"] = [](CallContext& c) {};
        f[std::string("SetStatic") + kTypes[i] + "Field"] = [](CallContext& c) {};
    }
}

struct Holder {
    std::mutex mutex;
    std::unique_ptr<Jni> jni;
};

}  // namespace

Jni& get(Runtime& rt) {
    auto& h = rt.state<Holder>();
    std::lock_guard lock(h.mutex);
    if (!h.jni) h.jni = std::make_unique<Jni>(rt);
    return *h.jni;
}

Jni::Jni(Runtime& rt) : rt_(rt) {
    // JNIEnv's function table.
    auto names = env_function_names();
    std::unordered_map<std::string, ShimFn> impls;
    register_env(impls);
    env_table_ = rt.heap().alloc_zeroed(8 * names.size());
    for (std::size_t i = 4; i < names.size(); ++i) {
        auto it = impls.find(names[i]);
        rt.mem().write<GuestAddr>(env_table_ + 8 * i, rt.make_thunk(names[i], it != impls.end() ? it->second : unimplemented));
    }

    // JavaVM: { reserved x3, DestroyJavaVM, AttachCurrentThread, DetachCurrentThread, GetEnv,
    // AttachCurrentThreadAsDaemon }.
    GuestAddr vm_table = rt.heap().alloc_zeroed(8 * 8);
    auto attach = [](CallContext& c) {
        c.mem().write<GuestAddr>(c.x(1), J(c).env(c.thread()));
        c.ret(kJniOk);
    };
    rt.mem().write<GuestAddr>(vm_table + 3 * 8, rt.make_thunk("DestroyJavaVM", [](CallContext& c) { c.ret(kJniOk); }));
    rt.mem().write<GuestAddr>(vm_table + 4 * 8, rt.make_thunk("AttachCurrentThread", attach));
    rt.mem().write<GuestAddr>(vm_table + 5 * 8, rt.make_thunk("DetachCurrentThread", [](CallContext& c) { c.ret(kJniOk); }));
    rt.mem().write<GuestAddr>(vm_table + 6 * 8, rt.make_thunk("GetEnv", attach));
    rt.mem().write<GuestAddr>(vm_table + 7 * 8, rt.make_thunk("AttachCurrentThreadAsDaemon", attach));
    vm_ = rt.heap().alloc_zeroed(16);
    rt.mem().write<GuestAddr>(vm_, vm_table);
}

GuestAddr Jni::env(GuestThread& thread) {
    std::lock_guard lock(mutex_);
    auto& e = envs_[thread.id];
    if (!e) {
        e = rt_.heap().alloc_zeroed(16);
        rt_.mem().write<GuestAddr>(e, env_table_);
    }
    return e;
}

GuestAddr Jni::add(Object o) {
    GuestAddr h = rt_.heap().alloc_zeroed(16);
    std::lock_guard lock(mutex_);
    objects_.emplace(h, std::move(o));
    return h;
}

GuestAddr Jni::find_class(const std::string& name) {
    {
        std::lock_guard lock(mutex_);
        if (auto it = classes_.find(name); it != classes_.end()) return it->second;
    }
    GuestAddr h = add({Object::Kind::Class, name});
    std::lock_guard lock(mutex_);
    return classes_.emplace(name, h).first->second;
}

GuestAddr Jni::new_string(const std::string& utf8) {
    Object o{Object::Kind::String, "java/lang/String"};
    o.text = utf8;
    return add(std::move(o));
}

GuestAddr Jni::new_instance(const std::string& class_name) { return add({Object::Kind::Instance, class_name}); }

GuestAddr Jni::new_array(char element, std::uint64_t length, const std::string& element_class) {
    Object o{Object::Kind::Array, std::string("[") + (element == 'L' ? "L" + element_class + ";" : std::string(1, element))};
    o.element = element;
    o.length = length;
    if (element == 'L')
        o.elements.assign(length, 0);
    else
        o.data = rt_.heap().alloc_zeroed(std::max<std::uint64_t>(1, length * element_size(element)));
    return add(std::move(o));
}

GuestAddr Jni::new_byte_buffer(GuestAddr address, std::uint64_t capacity) {
    Object o{Object::Kind::ByteBuffer, "java/nio/DirectByteBuffer"};
    o.data = address;
    o.length = capacity;
    return add(std::move(o));
}

void Jni::set_element(GuestAddr array, std::uint64_t index, GuestAddr value) {
    std::lock_guard lock(mutex_);
    auto it = objects_.find(array);
    if (it != objects_.end() && index < it->second.elements.size()) it->second.elements[index] = value;
}

std::optional<Object> Jni::object(GuestAddr handle) {
    std::lock_guard lock(mutex_);
    auto it = objects_.find(handle);
    if (it == objects_.end()) return std::nullopt;
    return it->second;
}

std::optional<Member> Jni::member(GuestAddr id) {
    std::lock_guard lock(mutex_);
    auto it = members_.find(id);
    if (it == members_.end()) return std::nullopt;
    return it->second;
}

GuestAddr Jni::member_id(Member m) {
    auto key = std::make_tuple(m.class_name, m.name, m.signature, m.is_static, m.is_field);
    {
        std::lock_guard lock(mutex_);
        if (auto it = member_ids_.find(key); it != member_ids_.end()) return it->second;
    }
    GuestAddr id = rt_.heap().alloc_zeroed(16);
    std::lock_guard lock(mutex_);
    members_.emplace(id, std::move(m));
    member_ids_.emplace(key, id);
    return id;
}

std::uint64_t Jni::default_value(const std::string& signature) {
    std::string type = return_type(signature);
    if (type.empty()) return 0;
    if (type == "Ljava/lang/String;") return new_string("");
    if (type[0] == 'L') return new_instance(type.substr(1, type.size() - 2));
    if (type[0] == '[') {
        char e = type.size() > 1 ? type[1] : 'B';
        return e == 'L' || e == '[' ? new_array('L', 0, "java/lang/Object") : new_array(e, 0);
    }
    return 0;
}

void Jni::implement(const std::string& class_name, const std::string& name, MethodImpl impl) {
    std::lock_guard lock(mutex_);
    impls_[{class_name, name}] = std::move(impl);
}

const MethodImpl* Jni::implementation(const Member& m) {
    std::lock_guard lock(mutex_);
    auto it = impls_.find({m.class_name, m.name});
    return it == impls_.end() ? nullptr : &it->second;
}

void Jni::register_natives(const std::string& class_name, std::vector<NativeMethod> methods) {
    log::debug("RegisterNatives: {} methods on {}", methods.size(), class_name);
    std::lock_guard lock(mutex_);
    auto& list = natives_[class_name];
    list.insert(list.end(), methods.begin(), methods.end());
}

std::optional<GuestAddr> Jni::native(const std::string& class_name, const std::string& name) {
    std::lock_guard lock(mutex_);
    auto it = natives_.find(class_name);
    if (it == natives_.end()) return std::nullopt;
    for (const auto& m : it->second)
        if (m.name == name) return m.fn;
    return std::nullopt;
}

}  // namespace rfu::jni
