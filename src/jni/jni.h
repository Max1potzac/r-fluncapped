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

// Answers a call to one Java method. Gets the shim context (x0 = env, x1 = object or
// class, x2 = method id, then the Java arguments) and returns the raw return value.
using MethodImpl = std::function<std::uint64_t(CallContext&, const Member&)>;

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

    void implement(const std::string& class_name, const std::string& name, MethodImpl impl);
    const MethodImpl* implementation(const Member& m);

    void register_natives(const std::string& class_name, std::vector<NativeMethod> methods);
    std::optional<GuestAddr> native(const std::string& class_name, const std::string& name);

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
