#pragma once

// The Java side of the process, as far as native code can see it.
//
// There is no JVM. Native code gets a JavaVM and JNIEnv whose functions are host shims
// working on a small object model: classes by name, strings, arrays, and plain instances.
// Calls into Java methods are logged and answered with neutral values (0, an empty string,
// a fresh instance of the declared return type) until a real implementation is registered
// for that method.

#include "core/guest_memory.h"

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rfu {

class Runtime;
class CallContext;
struct GuestThread;

namespace jni {

struct Object {
    enum class Kind { Class, String, Instance, Array, ByteBuffer } kind;
    std::string class_name = {};  // for a Class: the class it names; otherwise the object's class
    std::string text = {};        // String contents (UTF-8)
    std::uint64_t length = 0;      // Array length, ByteBuffer capacity
    char element = 0;              // Array element type: Z B C S I J F D, or L for objects
    GuestAddr data = 0;            // Array storage (primitive arrays), ByteBuffer address
    std::vector<GuestAddr> elements = {};  // object arrays
    std::map<std::string, std::uint64_t> fields = {};  // instance (or, on a Class, static) fields set so far
};

struct Member {
    std::string class_name, name, signature;
    bool is_static;
    bool is_field;
};

struct NativeMethod {
    std::string name, signature;
    GuestAddr fn;
};

class Jni;

// One call from native code into a Java method, constructor or field read.
struct JavaCall {
    CallContext& c;
    Jni& jni;
    const Member& member;
    GuestAddr self;                    // the object, or the class for static members
    std::vector<std::uint64_t> args;   // Java arguments: ints and references as they are, float/double bits

    std::string string_arg(std::size_t i) const;  // a java.lang.String argument ("" for null)
    float float_arg(std::size_t i) const;
    double double_arg(std::size_t i) const;
};

// Answers a call to one Java method and returns the raw return value (a reference, an
// integer, or the bits of a float/double).
using MethodImpl = std::function<std::uint64_t(JavaCall&)>;

// The parameter types of a method signature: "(ILjava/lang/String;[B)V" -> I, Ljava/lang/String;, [B
std::vector<std::string> parameter_types(const std::string& signature);

class Jni {
public:
    explicit Jni(Runtime& rt);

    GuestAddr vm() const { return vm_; }
    GuestAddr env(GuestThread& thread);

    GuestAddr find_class(const std::string& name);  // "com/ea/Foo"
    GuestAddr new_string(const std::string& utf8);
    GuestAddr new_instance(const std::string& class_name);
    GuestAddr new_array(char element, std::uint64_t length, const std::string& element_class = {});
    GuestAddr new_byte_buffer(GuestAddr address, std::uint64_t capacity);
    void set_element(GuestAddr array, std::uint64_t index, GuestAddr value);
    std::optional<Object> object(GuestAddr handle);
    std::optional<Member> member(GuestAddr id);
    GuestAddr member_id(Member m);

    // The neutral answer for a method or field of `signature`'s return type.
    std::uint64_t default_value(const std::string& signature);

    // Fields: what Get<Type>Field returns until native code or the host sets them.
    std::optional<std::uint64_t> field(GuestAddr object, const std::string& name);
    void set_field(GuestAddr object, const std::string& name, std::uint64_t value);
    std::string string_of(GuestAddr s);  // "" for null or a non-string
    GuestAddr array_data(GuestAddr array);

    // Replaces the logged default answer for every method (and constructor, "<init>") of
    // `class_name` called `name`, whatever its signature.
    void implement(const std::string& class_name, const std::string& name, MethodImpl impl);
    const MethodImpl* implementation(const Member& m);

    void register_natives(const std::string& class_name, std::vector<NativeMethod> methods);
    std::optional<GuestAddr> native(const std::string& class_name, const std::string& name);
    // Calls a native method the guest registered, as Java would: (env, this or class, args...).
    std::uint64_t call_native(const std::string& class_name, const std::string& name, GuestAddr self,
                              const std::vector<std::uint64_t>& args = {});

    Runtime& rt() { return rt_; }

private:
    GuestAddr add(Object o);

    Runtime& rt_;
    GuestAddr vm_ = 0;
    GuestAddr env_table_ = 0;
    std::mutex mutex_;
    std::unordered_map<GuestAddr, Object> objects_;
    std::unordered_map<std::string, GuestAddr> classes_;
    std::unordered_map<GuestAddr, Member> members_;
    std::map<std::tuple<std::string, std::string, std::string, bool, bool>, GuestAddr> member_ids_;
    std::unordered_map<std::uint32_t, GuestAddr> envs_;  // guest thread id -> JNIEnv*
    std::map<std::pair<std::string, std::string>, MethodImpl> impls_;
    std::map<std::string, std::vector<NativeMethod>> natives_;
};

// The runtime's Java world, created on first use.
Jni& get(Runtime& rt);

}  // namespace jni
}  // namespace rfu
