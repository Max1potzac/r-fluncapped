// <pthread.h> and <semaphore.h> on host threads.
//
// Guest synchronisation objects stay opaque guest memory; each maps (by address) to a host
// object created on first use, so statically initialised mutexes work without an init call.
// Only the mutex type is read from guest memory, where bionic's initialisers put it.

#include "core/log.h"
#include "shims/common.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <thread>
#include <unordered_map>

namespace rfu {

namespace {

using Clock = std::chrono::system_clock;

constexpr int kMutexNormal = 0, kMutexRecursive = 1;  // 2 is PTHREAD_MUTEX_ERRORCHECK
constexpr std::uint64_t kDefaultStack = 1ull << 20;

// Thrown by pthread_exit; caught at the top of the guest thread.
struct ThreadExit {
    GuestAddr value;
};

Clock::time_point deadline(const GuestMemory& mem, GuestAddr ts) {
    auto d = std::chrono::seconds(mem.read<std::int64_t>(ts)) + std::chrono::nanoseconds(mem.read<std::int64_t>(ts + 8));
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(d));
}

struct Mutex {
    std::mutex m;
    std::condition_variable cv;
    std::thread::id owner;
    std::uint32_t count = 0;
    int type = kMutexNormal;

    // Returns 0 or an Android errno. `until` = nullopt blocks forever.
    int lock(bool try_only, std::optional<Clock::time_point> until = std::nullopt) {
        auto me = std::this_thread::get_id();
        std::unique_lock l(m);
        if (count && owner == me) {
            if (type == kMutexRecursive) {
                ++count;
                return 0;
            }
            if (type == kMutexNormal) log::warn("guest thread relocked a non-recursive mutex it holds");
            return try_only ? android::kEBUSY : android::kEDEADLK;
        }
        if (try_only && count) return android::kEBUSY;
        auto free = [&] { return count == 0; };
        if (until) {
            if (!cv.wait_until(l, *until, free)) return android::kETIMEDOUT;
        } else {
            cv.wait(l, free);
        }
        owner = me;
        count = 1;
        return 0;
    }

    int unlock() {
        std::lock_guard l(m);
        if (!count || owner != std::this_thread::get_id()) return android::kEPERM;
        if (--count == 0) {
            owner = {};
            cv.notify_one();
        }
        return 0;
    }

    // For condition waits: give the mutex up entirely, then take it back the same depth.
    std::uint32_t release_all() {
        std::lock_guard l(m);
        std::uint32_t depth = count;
        count = 0;
        owner = {};
        cv.notify_one();
        return depth;
    }
    void reacquire(std::uint32_t depth) {
        std::unique_lock l(m);
        cv.wait(l, [&] { return count == 0; });
        owner = std::this_thread::get_id();
        count = depth;
    }
};

struct Cond {
    std::mutex m;
    std::condition_variable cv;
    std::uint64_t seq = 0;
};

struct RwLock {
    std::mutex m;
    std::condition_variable cv;
    std::uint32_t readers = 0;
    bool writing = false;
    std::thread::id writer;
};

struct Semaphore {
    std::mutex m;
    std::condition_variable cv;
    std::uint32_t count = 0;
};

struct HostThread {
    std::thread thread;
    GuestAddr result = 0;
    bool detached = false;
};

struct Key {
    bool used = false;
    GuestAddr destructor = 0;
};

struct Pthreads {
    std::mutex mutex;
    std::unordered_map<GuestAddr, std::shared_ptr<Mutex>> mutexes;
    std::unordered_map<GuestAddr, std::shared_ptr<Cond>> conds;
    std::unordered_map<GuestAddr, std::shared_ptr<RwLock>> rwlocks;
    std::unordered_map<GuestAddr, std::shared_ptr<Semaphore>> sems;
    std::unordered_map<GuestAddr, std::shared_ptr<HostThread>> threads;
    std::vector<Key> keys;
    std::recursive_mutex once;

    template <typename T, typename Init>
    std::shared_ptr<T> get(std::unordered_map<GuestAddr, std::shared_ptr<T>>& table, GuestAddr a, Init init) {
        std::lock_guard l(mutex);
        auto& slot = table[a];
        if (!slot) {
            slot = std::make_shared<T>();
            init(*slot);
        }
        return slot;
    }
    template <typename T>
    void erase(std::unordered_map<GuestAddr, std::shared_ptr<T>>& table, GuestAddr a) {
        std::lock_guard l(mutex);
        table.erase(a);
    }
};

Pthreads& pt(CallContext& c) { return c.rt().state<Pthreads>(); }

std::shared_ptr<Mutex> mutex_at(CallContext& c, GuestAddr a) {
    // bionic keeps the type in bits 14-15 of the first word (see PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP).
    return pt(c).get(pt(c).mutexes, a, [&](Mutex& m) { m.type = (c.mem().read<std::uint32_t>(a) >> 14) & 3; });
}
std::shared_ptr<Cond> cond_at(CallContext& c, GuestAddr a) { return pt(c).get(pt(c).conds, a, [](Cond&) {}); }
std::shared_ptr<RwLock> rwlock_at(CallContext& c, GuestAddr a) { return pt(c).get(pt(c).rwlocks, a, [](RwLock&) {}); }
std::shared_ptr<Semaphore> sem_at(CallContext& c, GuestAddr a) {
    return pt(c).get(pt(c).sems, a, [&](Semaphore& s) { s.count = c.mem().read<std::uint32_t>(a); });
}

void cond_wait(CallContext& c, std::optional<Clock::time_point> until) {
    auto cond = cond_at(c, c.x(0));
    auto m = mutex_at(c, c.x(1));
    std::unique_lock l(cond->m);
    std::uint64_t seq = cond->seq;
    std::uint32_t depth = m->release_all();
    bool timed_out = false;
    if (until)
        timed_out = !cond->cv.wait_until(l, *until, [&] { return cond->seq != seq; });
    else
        cond->cv.wait(l, [&] { return cond->seq != seq; });
    l.unlock();
    m->reacquire(depth);
    c.ret(timed_out ? android::kETIMEDOUT : 0);
}

void run_key_destructors(Runtime& rt, GuestThread& t) {
    auto& p = rt.state<Pthreads>();
    for (int round = 0; round < 4; ++round) {
        bool any = false;
        for (auto& [key, value] : std::unordered_map(t.specific)) {
            GuestAddr dtor;
            {
                std::lock_guard l(p.mutex);
                dtor = key < p.keys.size() && p.keys[key].used ? p.keys[key].destructor : 0;
            }
            if (!value || !dtor) continue;
            t.specific[key] = 0;
            t.cpu->call(dtor, {value});
            any = true;
        }
        if (!any) break;
    }
}

}  // namespace

void register_libc_pthread(ShimRegistry& reg) {
    // --- threads -------------------------------------------------------------------------
    reg.add("pthread_create", [](CallContext& c) {
        std::uint64_t stack = kDefaultStack;
        bool detached = false;
        if (GuestAddr attr = c.x(1)) {
            stack = std::max<std::uint64_t>(c.mem().read<std::uint64_t>(attr + 16), 64 * 1024);
            detached = c.mem().read<std::uint32_t>(attr) & 1;
        }
        Runtime& rt = c.rt();
        auto t = rt.create_thread(stack);
        auto host = std::make_shared<HostThread>();
        host->detached = detached;
        {
            auto& p = pt(c);
            std::lock_guard l(p.mutex);
            p.threads[t->handle] = host;
        }
        GuestAddr start = c.x(2), arg = c.x(3);
        c.mem().write<GuestAddr>(c.x(0), t->handle);
        log::debug("pthread_create: thread {} at {:#x}", t->id, start);
        host->thread = std::thread([&rt, t, host, start, arg] {
            rt.bind_thread(t);
            GuestAddr result = 0;
            try {
                result = t->cpu->call(start, {arg});
            } catch (const ThreadExit& e) {
                result = e.value;
            } catch (const GuestFault& e) {
                log::error("guest thread {} faulted: {}", t->id, e.what());
            } catch (const GuestExit& e) {
                log::info("guest thread {} called exit({})", t->id, e.code);
            }
            try {
                run_key_destructors(rt, *t);
            } catch (const std::exception& e) {
                log::error("thread {} key destructor failed: {}", t->id, e.what());
            }
            host->result = result;
            rt.unbind_thread();
        });
        if (detached) host->thread.detach();
        c.ret(0);
    });
    reg.add("pthread_join", [](CallContext& c) {
        std::shared_ptr<HostThread> host;
        {
            auto& p = pt(c);
            std::lock_guard l(p.mutex);
            auto it = p.threads.find(c.x(0));
            if (it == p.threads.end()) return c.ret(3);  // ESRCH
            host = it->second;
            p.threads.erase(it);
        }
        if (host->detached || !host->thread.joinable()) return c.ret(android::kEINVAL);
        host->thread.join();
        if (c.x(1)) c.mem().write<GuestAddr>(c.x(1), host->result);
        c.ret(0);
    });
    reg.add("pthread_detach", [](CallContext& c) {
        auto& p = pt(c);
        std::lock_guard l(p.mutex);
        auto it = p.threads.find(c.x(0));
        if (it == p.threads.end()) return c.ret(3);
        if (it->second->thread.joinable()) it->second->thread.detach();
        it->second->detached = true;
        p.threads.erase(it);
        c.ret(0);
    });
    reg.add("pthread_exit", [](CallContext& c) {
        if (c.thread().id == 1) c.rt().exit(0);
        throw ThreadExit{c.x(0)};
    });
    reg.add("pthread_self", [](CallContext& c) { c.ret(c.thread().handle); });
    reg.add("pthread_equal", [](CallContext& c) { c.ret(c.x(0) == c.x(1) ? 1 : 0); });

    // pthread_attr_t: u32 flags, void* stack_base, size_t stack_size, size_t guard_size,
    // s32 sched_policy, s32 sched_priority.
    reg.add("pthread_attr_init", [](CallContext& c) {
        std::memset(c.ptr(0), 0, 56);
        c.mem().write<std::uint64_t>(c.x(0) + 16, kDefaultStack);
        c.mem().write<std::uint64_t>(c.x(0) + 24, GuestMemory::kPageSize);
        c.ret(0);
    });
    reg.add("pthread_attr_destroy", [](CallContext& c) { c.ret(0); });
    reg.add("pthread_attr_setdetachstate", [](CallContext& c) {
        std::uint32_t flags = c.mem().read<std::uint32_t>(c.x(0));
        c.mem().write<std::uint32_t>(c.x(0), c.x(1) ? flags | 1 : flags & ~1u);
        c.ret(0);
    });
    reg.add("pthread_attr_setstacksize", [](CallContext& c) {
        c.mem().write<std::uint64_t>(c.x(0) + 16, c.x(1));
        c.ret(0);
    });
    reg.add("pthread_attr_setstack", [](CallContext& c) {
        // The guest supplies the stack; we always allocate our own, but keep its size.
        c.mem().write<std::uint64_t>(c.x(0) + 8, c.x(1));
        c.mem().write<std::uint64_t>(c.x(0) + 16, c.x(2));
        c.ret(0);
    });
    reg.add("pthread_attr_getstack", [](CallContext& c) {
        c.mem().write<std::uint64_t>(c.x(1), c.mem().read<std::uint64_t>(c.x(0) + 8));
        c.mem().write<std::uint64_t>(c.x(2), c.mem().read<std::uint64_t>(c.x(0) + 16));
        c.ret(0);
    });
    reg.add("pthread_getattr_np", [](CallContext& c) {
        GuestThread* t = c.x(0) == c.thread().handle ? &c.thread() : c.rt().thread_by_handle(c.x(0));
        if (!t) return c.ret(3);
        std::memset(c.mem().host(c.x(1)), 0, 56);
        c.mem().write<std::uint64_t>(c.x(1) + 8, t->stack_lo);
        c.mem().write<std::uint64_t>(c.x(1) + 16, t->stack_hi - t->stack_lo);
        c.mem().write<std::uint64_t>(c.x(1) + 24, GuestMemory::kPageSize);
        c.ret(0);
    });
    for (const char* name : {"pthread_attr_setschedparam", "pthread_attr_setschedpolicy", "pthread_setschedparam"})
        reg.add(name, [](CallContext& c) { c.ret(0); });
    reg.add("pthread_attr_getschedparam", [](CallContext& c) {
        c.mem().write<std::int32_t>(c.x(1), 0);
        c.ret(0);
    });
    reg.add("pthread_getschedparam", [](CallContext& c) {
        if (c.x(1)) c.mem().write<std::int32_t>(c.x(1), 0);  // SCHED_OTHER
        if (c.x(2)) c.mem().write<std::int32_t>(c.x(2), 0);
        c.ret(0);
    });
    reg.add("pthread_atfork", [](CallContext& c) { c.ret(0); });

    // --- mutexes -------------------------------------------------------------------------
    reg.add("pthread_mutexattr_init", [](CallContext& c) {
        c.mem().write<std::uint64_t>(c.x(0), 0);
        c.ret(0);
    });
    reg.add("pthread_mutexattr_destroy", [](CallContext& c) { c.ret(0); });
    reg.add("pthread_mutexattr_settype", [](CallContext& c) {
        if (c.x(1) > 2) return c.ret(android::kEINVAL);
        c.mem().write<std::uint64_t>(c.x(0), (c.mem().read<std::uint64_t>(c.x(0)) & ~3ull) | c.x(1));
        c.ret(0);
    });
    reg.add("pthread_mutexattr_setpshared", [](CallContext& c) { c.ret(0); });
    reg.add("pthread_mutex_init", [](CallContext& c) {
        std::uint32_t type = c.x(1) ? static_cast<std::uint32_t>(c.mem().read<std::uint64_t>(c.x(1)) & 3) : 0;
        std::memset(c.ptr(0), 0, 40);
        c.mem().write<std::uint32_t>(c.x(0), type << 14);
        pt(c).erase(pt(c).mutexes, c.x(0));
        c.ret(0);
    });
    reg.add("pthread_mutex_destroy", [](CallContext& c) {
        pt(c).erase(pt(c).mutexes, c.x(0));
        c.ret(0);
    });
    reg.add("pthread_mutex_lock", [](CallContext& c) { c.ret(static_cast<std::uint64_t>(mutex_at(c, c.x(0))->lock(false))); });
    reg.add("pthread_mutex_trylock", [](CallContext& c) { c.ret(static_cast<std::uint64_t>(mutex_at(c, c.x(0))->lock(true))); });
    reg.add("pthread_mutex_unlock", [](CallContext& c) { c.ret(static_cast<std::uint64_t>(mutex_at(c, c.x(0))->unlock())); });

    // --- condition variables ---------------------------------------------------------------
    reg.add("pthread_condattr_init", [](CallContext& c) {
        c.mem().write<std::uint64_t>(c.x(0), 0);
        c.ret(0);
    });
    reg.add("pthread_condattr_destroy", [](CallContext& c) { c.ret(0); });
    reg.add("pthread_cond_init", [](CallContext& c) {
        pt(c).erase(pt(c).conds, c.x(0));
        c.ret(0);
    });
    reg.add("pthread_cond_destroy", [](CallContext& c) {
        pt(c).erase(pt(c).conds, c.x(0));
        c.ret(0);
    });
    reg.add("pthread_cond_signal", [](CallContext& c) {
        auto cond = cond_at(c, c.x(0));
        std::lock_guard l(cond->m);
        ++cond->seq;
        cond->cv.notify_one();
        c.ret(0);
    });
    reg.add("pthread_cond_broadcast", [](CallContext& c) {
        auto cond = cond_at(c, c.x(0));
        std::lock_guard l(cond->m);
        ++cond->seq;
        cond->cv.notify_all();
        c.ret(0);
    });
    reg.add("pthread_cond_wait", [](CallContext& c) { cond_wait(c, std::nullopt); });
    reg.add("pthread_cond_timedwait", [](CallContext& c) { cond_wait(c, deadline(c.mem(), c.x(2))); });

    // --- read/write locks ------------------------------------------------------------------
    reg.add("pthread_rwlock_init", [](CallContext& c) {
        pt(c).erase(pt(c).rwlocks, c.x(0));
        c.ret(0);
    });
    reg.add("pthread_rwlock_destroy", [](CallContext& c) {
        pt(c).erase(pt(c).rwlocks, c.x(0));
        c.ret(0);
    });
    reg.add("pthread_rwlock_rdlock", [](CallContext& c) {
        auto rw = rwlock_at(c, c.x(0));
        std::unique_lock l(rw->m);
        rw->cv.wait(l, [&] { return !rw->writing; });
        ++rw->readers;
        c.ret(0);
    });
    reg.add("pthread_rwlock_wrlock", [](CallContext& c) {
        auto rw = rwlock_at(c, c.x(0));
        std::unique_lock l(rw->m);
        rw->cv.wait(l, [&] { return !rw->writing && rw->readers == 0; });
        rw->writing = true;
        rw->writer = std::this_thread::get_id();
        c.ret(0);
    });
    reg.add("pthread_rwlock_unlock", [](CallContext& c) {
        auto rw = rwlock_at(c, c.x(0));
        std::lock_guard l(rw->m);
        if (rw->writing && rw->writer == std::this_thread::get_id()) rw->writing = false;
        else if (rw->readers) --rw->readers;
        rw->cv.notify_all();
        c.ret(0);
    });

    // --- once and keys -----------------------------------------------------------------------
    reg.add("pthread_once", [](CallContext& c) {
        constexpr std::int32_t kDone = 2;
        GuestAddr control = c.x(0);
        if (c.mem().read<std::int32_t>(control) == kDone) return c.ret(0);
        std::lock_guard l(pt(c).once);
        if (c.mem().read<std::int32_t>(control) != kDone) {
            c.mem().write<std::int32_t>(control, 1);
            c.cpu().call(c.x(1), {});
            c.mem().write<std::int32_t>(control, kDone);
        }
        c.ret(0);
    });
    reg.add("pthread_key_create", [](CallContext& c) {
        auto& p = pt(c);
        std::lock_guard l(p.mutex);
        std::size_t k = 0;
        while (k < p.keys.size() && p.keys[k].used) ++k;
        if (k == p.keys.size()) p.keys.emplace_back();
        p.keys[k] = {true, c.x(1)};
        c.mem().write<std::uint32_t>(c.x(0), static_cast<std::uint32_t>(k));
        c.ret(0);
    });
    reg.add("pthread_key_delete", [](CallContext& c) {
        auto& p = pt(c);
        std::lock_guard l(p.mutex);
        if (c.x(0) < p.keys.size()) p.keys[c.x(0)] = {};
        c.ret(0);
    });
    reg.add("pthread_getspecific", [](CallContext& c) {
        auto& s = c.thread().specific;
        auto it = s.find(static_cast<std::uint32_t>(c.x(0)));
        c.ret(it == s.end() ? 0 : it->second);
    });
    reg.add("pthread_setspecific", [](CallContext& c) {
        c.thread().specific[static_cast<std::uint32_t>(c.x(0))] = c.x(1);
        c.ret(0);
    });

    // --- semaphores ----------------------------------------------------------------------
    reg.add("sem_init", [](CallContext& c) {
        pt(c).erase(pt(c).sems, c.x(0));
        c.mem().write<std::uint32_t>(c.x(0), static_cast<std::uint32_t>(c.x(2)));
        c.ret(0);
    });
    reg.add("sem_destroy", [](CallContext& c) {
        pt(c).erase(pt(c).sems, c.x(0));
        c.ret(0);
    });
    reg.add("sem_post", [](CallContext& c) {
        auto s = sem_at(c, c.x(0));
        std::lock_guard l(s->m);
        ++s->count;
        s->cv.notify_one();
        c.ret(0);
    });
    reg.add("sem_wait", [](CallContext& c) {
        auto s = sem_at(c, c.x(0));
        std::unique_lock l(s->m);
        s->cv.wait(l, [&] { return s->count > 0; });
        --s->count;
        c.ret(0);
    });
    reg.add("sem_trywait", [](CallContext& c) {
        auto s = sem_at(c, c.x(0));
        std::lock_guard l(s->m);
        if (!s->count) return c.fail(android::kEAGAIN);
        --s->count;
        c.ret(0);
    });
    reg.add("sem_timedwait", [](CallContext& c) {
        auto s = sem_at(c, c.x(0));
        std::unique_lock l(s->m);
        if (!s->cv.wait_until(l, deadline(c.mem(), c.x(1)), [&] { return s->count > 0; })) return c.fail(android::kETIMEDOUT);
        --s->count;
        c.ret(0);
    });
    reg.add("sem_getvalue", [](CallContext& c) {
        auto s = sem_at(c, c.x(0));
        std::lock_guard l(s->m);
        c.mem().write<std::int32_t>(c.x(1), static_cast<std::int32_t>(s->count));
        c.ret(0);
    });
}

}  // namespace rfu
