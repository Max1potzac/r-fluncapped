#pragma once

// --trace-class: logs calls to a class's virtual functions, found by RTTI (core/rtti.h), to
// learn what the game's unnamed code does: run with it, do something in the game (tap a
// sun), and the log shows which functions ran around that time. Only calls on objects of the
// class itself count, and each function is logged on its 1st, 2nd, 3rd, 10th, 100th... call,
// so per-frame ones don't flood the log. With RFU_TRACE_X1=1 in the environment each logged call
// also shows the 64 bytes its second argument points at (a touch, a point...).

#include <string>

namespace rfu {

class Runtime;
class Module;

// Returns false (and logs why) when the class isn't found.
bool trace_class(Runtime& rt, const Module& module, const std::string& class_name);

// True once anything is traced: the app then logs touches too, to line them up with calls.
bool tracing();

}  // namespace rfu
