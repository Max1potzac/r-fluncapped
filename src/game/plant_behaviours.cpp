#include "game/plant_behaviours.h"

#include "core/hooks.h"
#include "core/log.h"
#include "core/module.h"
#include "cpu/cpu.h"
#include "runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

namespace rfu {

namespace {

// libPVZ2.so offsets, with the first instructions expected there.
struct Known {
    std::uint64_t offset;
    std::array<std::uint32_t, 3> code;
};
// Projectile::Hit(projectile, target), most projectiles' (a PrimalPeashooterProjectile knocks back,
// then calls it), after the damage is done.
constexpr Known kHit{0xca065c, {0xD10103FF, 0xF9000BF5, 0xA9024FF4}};
// Board::AddProjectile(board, const props handle&, instigator, position), which plants' actions
// fire through (Plant::DoAction, which RFLib replaces with its own).
constexpr Known kAddProjectile{0xaae5b4, {0xD10283FF, 0x6D032BEB, 0x6D0423E9}};
constexpr Known kLeafUpdate{0xee194c, {0xD10283FF, 0x6D033BEF, 0x6D0433ED}};  // HomingThistleLeaf's per-frame update
constexpr Known kLeafSetTarget{0xee05e8, {0xD10143FF, 0xF90013F5, 0xA9034FF4}};  // (leaf, zombie or null)
// RtWeakPtr: parse "RTID(name@sheet)" (x0 a std::string, the handle through x8).
constexpr Known kParseRtid{0x14daf88, {0xD104C3FF, 0xA90D6FFC, 0xA90E67FA}};
// Handles to objects, as game_keys looks them up.
constexpr Known kRegistry{0x14ead90, {0xD10143FF, 0xF90013F5, 0xA9034FF4}};
constexpr Known kHandleRef{0x14f39cc, {0xA9BD57F6, 0xA9014FF4, 0xA9027BFD}};
constexpr Known kRefIsNull{0x14d926c, {0xF9400008, 0xF100011F, 0x1A9F17E0}};
constexpr Known kRefType{0x14d9178, {0xF9400008, 0xD371F900, 0xD65F03C0}};
constexpr Known kTypeTable{0x14f31a4, {0x91006008, 0x7213003F, 0x9A800109}};
constexpr Known kRefResolve{0x14ede00, {0xF81C0FF7, 0xA90157F6, 0xA9024FF4}};
constexpr Known kRefFree{0x14d9154, {0xA9BF7BFD, 0x910003FD, 0xAA1F03E1}};
// The game's objects of a type, the way plants look for targets (PlantFramework's slot 20): an
// iterator from the object lists (a lazy singleton), then has-next, current (a handle through x8),
// next, done.
constexpr std::uint64_t kObjectLists = 0x256a0d8;
constexpr std::uint32_t kZombieType = 0x43;
constexpr Known kIterate{0x11beef0, {0xA9BE4FF4, 0xA9017BFD, 0x910043FD}};
constexpr Known kIterHasNext{0x14ea3cc, {0xB9401808, 0xB9401009, 0x6B09011F}};
constexpr Known kIterCurrent{0x14ea364, {0xF9400409, 0xB40000E9, 0x2942AC0A}};
constexpr Known kIterNext{0x14ea3b0, {0xB9401808, 0xB9401009, 0x1100050A}};
constexpr Known kIterDone{0x14ea1fc, {0xA9BD57F6, 0xA9014FF4, 0xA9027BFD}};
// Zombie: dead or dying; whether a plant's targeting skips it (flags, plant or null: 1 skips; RFLib
// puts its own in its place, so its code isn't checked);
// Zombie's RtClass, for RtObject::IsA (vtable slot 4).
constexpr Known kZombieDead{0xc3e204, {0xB940B008, 0x51000D09, 0x7100313F}};
constexpr Known kSkipTarget{0xc56b90, {0xD10183FF, 0xF90013F7, 0xA90357F6}};
constexpr Known kZombieClass{0xc36fe4, {0xF81D0FF5, 0xA9014FF4, 0xA9027BFD}};
constexpr std::uint64_t kIsASlot = 0x20;
constexpr std::uint32_t kTargetFlags = 0xa85;  // Homing Thistle's plant food's (0x800 | 0x285)
// The checks Homing Thistle's leaf drops its target on, besides being dead.
constexpr std::uint64_t kZombieGone1 = 0x171, kZombieGone1Bit = 4, kZombieGone2 = 0x172, kZombieGone2Bit = 2;

// GameObject: its position (x towards the zombies' side). Projectile: its properties
// (resolved), who fired it, the damage it does. Plant: in plant food. Zombie: hit points.
// HomingThistleLeaf: its target.
constexpr std::uint64_t kPosX = 0x18;
constexpr std::uint64_t kProps = 0x58, kPropsHandle = 0x60, kInstigator = 0x68, kDamage = 236;
constexpr std::uint64_t kPlantFood = 296;
constexpr std::uint64_t kHitpoints = 376;
constexpr std::uint64_t kLeafTarget = 0x178;
// The lawn's right edge: the board's grid origin and size (ints, world units).
constexpr std::uint64_t kGame = 0x2593760, kBoardOfGame = 0x9a8, kGridOrigin = 0x440, kGridSize = 0x448;
constexpr float kPastEdge = 40;  // a zombie this far past the last column is on the lawn

constexpr auto kInFlight = std::chrono::milliseconds(300);  // a leaf not updated for this long is gone

class Behaviours : public std::enable_shared_from_this<Behaviours> {
public:
    Behaviours(Runtime& rt, GuestAddr base, const PlantBehaviours& b) : rt_(rt), base_(base) {
        for (const auto& k : b.kill_shots) kill_shots_.push_back({sheet(k.normal), sheet(k.special)});
        for (const auto& h : b.homing) homing_.push_back(sheet(h));
        scratch_ = rt.heap().alloc_zeroed(0x100);
        ref_ = scratch_;
        handle_ = scratch_ + 0x10;
        iter_ = scratch_ + 0x40;
    }

    void install() {
        auto self = shared_from_this();
        hook(rt_, base_ + kHit.offset, "kill shot: hit", [self](CallContext& c) { self->on_hit(c.x(0), c.x(1)); });
        if (!kill_shots_.empty()) {
            hook(rt_, base_ + kAddProjectile.offset, "kill shot: fire", [self](CallContext& c) { self->on_fire(c, c.x(1), c.x(2)); });
        }
        if (!homing_.empty())
            hook(rt_, base_ + kLeafUpdate.offset, "homing retarget", [self](CallContext& c) { self->on_leaf_update(c.x(0)); });
    }

private:
    // A projectile type by its alias: the handle, made once the game is up, and the properties
    // object, once the sheet is loaded.
    struct Sheet {
        std::string rtid;
        GuestAddr handle = 0;  // a guest RtWeakPtr, kept
        GuestAddr object = 0;
    };
    struct KillShot {
        Sheet normal, special;
    };
    struct Chase {
        GuestAddr zombie;
        float damage;
        std::chrono::steady_clock::time_point seen;
    };

    static Sheet sheet(const std::string& alias) { return Sheet{"RTID(" + alias + "@ProjectileTypes)"}; }

    std::uint64_t call(const Known& k, const std::vector<std::uint64_t>& args = {}, std::optional<std::uint64_t> x8 = std::nullopt) {
        return rt_.call(base_ + k.offset, args, x8);
    }

    // The object a handle names, or 0.
    GuestAddr resolve(GuestAddr handle) {
        GuestAddr registry = call(kRegistry);
        call(kHandleRef, {registry, handle}, ref_);
        GuestAddr object = 0;
        if (!(call(kRefIsNull, {ref_}) & 1)) {
            std::uint64_t type = call(kRefType, {ref_});
            GuestAddr table = call(kTypeTable, {registry, type & 0xffffffff});
            if (table) object = call(kRefResolve, {table, ref_});
        }
        call(kRefFree, {ref_});
        return object;
    }

    // The properties object of a sheet, made and looked up the first time it's there.
    GuestAddr object(Sheet& s) {
        if (s.object) return s.object;
        auto& mem = rt_.mem();
        if (!s.handle) {
            // A libc++ std::string, long: capacity | 1, size, data.
            GuestAddr text = rt_.heap().alloc_zeroed(s.rtid.size() + 1), str = rt_.heap().alloc_zeroed(24);
            std::memcpy(mem.host(text), s.rtid.data(), s.rtid.size());
            mem.write<std::uint64_t>(str, (s.rtid.size() + 1) | 1);
            mem.write<std::uint64_t>(str + 8, s.rtid.size());
            mem.write<std::uint64_t>(str + 16, text);
            s.handle = rt_.heap().alloc_zeroed(8);
            call(kParseRtid, {str}, s.handle);
        }
        s.object = resolve(s.handle);
        if (s.object) log::debug("plant behaviours: {} is at {:#x}", s.rtid, s.object);
        return s.object;
    }

    GuestAddr props_of(GuestAddr projectile) {
        GuestAddr props = rt_.mem().read<std::uint64_t>(projectile + kProps);
        return props ? props : resolve(projectile + kPropsHandle);
    }

    bool is_zombie(GuestAddr object) {
        if (!object) return false;
        if (!zombie_class_) zombie_class_ = call(kZombieClass);
        GuestAddr is_a = rt_.mem().read<std::uint64_t>(rt_.mem().read<std::uint64_t>(object) + kIsASlot);
        return rt_.call(is_a, {object, zombie_class_}) & 1;
    }

    float hitpoints(GuestAddr zombie) { return rt_.mem().read<float>(zombie + kHitpoints); }
    bool dead(GuestAddr zombie) { return call(kZombieDead, {zombie}) & 1; }

    // A kill-shot projectile hits, its damage already done: the special one disarms its plant, and
    // one that left the zombie dead arms it.
    void on_hit(GuestAddr projectile, GuestAddr target) {
        if (!homing_.empty()) chasing_.erase(projectile);
        if (kill_shots_.empty()) return;
        GuestAddr props = props_of(projectile);
        if (!props) return;
        bool special = false, ours = false;
        for (auto& k : kill_shots_) {
            if (props == object(k.special)) special = ours = true;
            else if (props == object(k.normal)) ours = true;
        }
        if (!ours) return;
        GuestAddr plant = resolve(projectile + kInstigator);
        if (!plant) return;
        if (special && armed_.erase(plant)) log::debug("kill shot: plant {:#x} disarmed", plant);
        if (is_zombie(target) && (hitpoints(target) <= 0 || dead(target)) && armed_.insert(plant).second)
            log::debug("kill shot: plant {:#x} armed", plant);
    }

    // An armed plant fires its normal projectile: the special one instead.
    void on_fire(CallContext& c, GuestAddr handle, GuestAddr plant) {
        if (armed_.empty() || !armed_.count(plant) || rt_.mem().read<std::uint8_t>(plant + kPlantFood)) return;
        GuestAddr props = resolve(handle);
        for (auto& k : kill_shots_)
            if (props && props == object(k.normal) && object(k.special)) {
                c.cpu().jit().SetRegister(1, k.special.handle);  // the handle is the sheet's own: point elsewhere
                return;
            }
    }

    bool chases(GuestAddr leaf) {
        GuestAddr props = props_of(leaf);
        for (auto& h : homing_)
            if (props && props == object(h)) return true;
        return false;
    }

    bool targetable(GuestAddr zombie, float edge) {
        auto& mem = rt_.mem();
        if (dead(zombie) || hitpoints(zombie) <= 0) return false;
        if ((mem.read<std::uint8_t>(zombie + kZombieGone1) >> kZombieGone1Bit) & 1) return false;
        if ((mem.read<std::uint8_t>(zombie + kZombieGone2) >> kZombieGone2Bit) & 1) return false;
        if (mem.read<float>(zombie + kPosX) > edge) return false;
        return !(call(kSkipTarget, {zombie, kTargetFlags, 0}) & 1);
    }

    // The zombies a homing projectile can go for, nearest the house first.
    std::vector<std::pair<float, GuestAddr>> targets() {
        std::vector<std::pair<float, GuestAddr>> out;
        auto& mem = rt_.mem();
        GuestAddr lists = mem.read<std::uint64_t>(base_ + kObjectLists);
        GuestAddr game = mem.read<std::uint64_t>(base_ + kGame);
        GuestAddr board = game ? mem.read<std::uint64_t>(game + kBoardOfGame) : 0;
        if (!lists || !board) return out;
        float edge = static_cast<float>(mem.read<std::int32_t>(board + kGridOrigin) + mem.read<std::int32_t>(board + kGridSize)) + kPastEdge;
        call(kIterate, {lists, kZombieType}, iter_);
        while (call(kIterHasNext, {iter_}) & 1) {
            call(kIterCurrent, {iter_}, handle_);
            GuestAddr z = resolve(handle_);
            call(kRefFree, {handle_});
            if (z && targetable(z, edge)) out.emplace_back(mem.read<float>(z + kPosX), z);
            call(kIterNext, {iter_});
        }
        call(kIterDone, {iter_});
        std::sort(out.begin(), out.end());
        return out;
    }

    void on_leaf_update(GuestAddr leaf) {
        if (!chases(leaf)) return;
        auto& mem = rt_.mem();
        auto now = std::chrono::steady_clock::now();
        std::erase_if(chasing_, [&](const auto& e) { return now - e.second.seen > kInFlight; });
        float damage = mem.read<float>(leaf + kDamage);
        GuestAddr current = resolve(leaf + kLeafTarget);
        if (current && targetable(current, INFINITY)) {
            chasing_[leaf] = {current, damage, now};
            return;
        }
        chasing_.erase(leaf);
        auto all = targets();
        if (all.empty()) return;
        // The nearest one the leaves already chasing it won't finish, else the nearest.
        GuestAddr pick = all.front().second;
        for (const auto& [x, z] : all) {
            float coming = 0;
            for (const auto& [other, chase] : chasing_)
                if (chase.zombie == z) coming += chase.damage;
            if (hitpoints(z) > coming) {
                pick = z;
                break;
            }
        }
        call(kLeafSetTarget, {leaf, pick});
        chasing_[leaf] = {pick, damage, now};
        log::debug("homing retarget: {:#x} goes for zombie {:#x} (x {:.0f}, {:.0f} hp){}", leaf, pick, mem.read<float>(pick + kPosX), hitpoints(pick),
                   current ? ", its target gone" : "");
    }

    Runtime& rt_;
    GuestAddr base_;
    std::vector<KillShot> kill_shots_;
    std::vector<Sheet> homing_;
    GuestAddr scratch_ = 0, ref_ = 0, handle_ = 0, iter_ = 0, zombie_class_ = 0;
    std::set<GuestAddr> armed_;  // plants whose next shot is the special one
    std::unordered_map<GuestAddr, Chase> chasing_;
};

bool code_matches(Runtime& rt, GuestAddr addr, const std::array<std::uint32_t, 3>& code) {
    for (std::size_t i = 0; i < code.size(); ++i)
        if (rt.mem().read<std::uint32_t>(addr + 4 * i) != code[i]) return false;
    return true;
}

}  // namespace

bool parse_kill_shot(const std::string& text, PlantBehaviours::KillShot& out, std::string& error) {
    auto eq = text.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == text.size()) {
        error = "--kill-shot wants <normal projectile>=<special projectile>, got \"" + text + "\"";
        return false;
    }
    out = {text.substr(0, eq), text.substr(eq + 1)};
    return true;
}

bool install_plant_behaviours(Runtime& rt, const Module& pvz2, const PlantBehaviours& behaviours) {
    if (behaviours.empty()) return false;
    GuestAddr base = pvz2.base();
    for (const Known* k : {&kHit, &kAddProjectile, &kLeafUpdate, &kLeafSetTarget, &kParseRtid, &kRegistry,
                           &kHandleRef, &kRefIsNull, &kRefType, &kTypeTable, &kRefResolve, &kRefFree, &kIterate, &kIterHasNext,
                           &kIterCurrent, &kIterNext, &kIterDone, &kZombieDead, &kZombieClass})
        if (!code_matches(rt, base + k->offset, k->code)) {
            log::warn("plant behaviours (--kill-shot, --homing-retarget): this libPVZ2.so is a build they don't know ({:#x} differs); left off",
                      k->offset);
            return false;
        }
    std::make_shared<Behaviours>(rt, base, behaviours)->install();
    for (const auto& k : behaviours.kill_shots) log::info("kill shot: {} arms {}", k.normal, k.special);
    for (const auto& h : behaviours.homing) log::info("homing retarget: {}", h);
    return true;
}

}  // namespace rfu
