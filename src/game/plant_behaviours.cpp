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
#include <map>
#include <memory>
#include <optional>
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
// PlantAnimRig starting its attack animation: the call that plays it, the rig in x0 and the label
// (a std::string on the stack, "attack") in x1, three instructions on.
constexpr Known kRigPlayAttack{0x5c6024, {0x9100C3E3, 0xAA1303E0, 0x2A1F03E2}};
constexpr std::uint64_t kRigPlayAttackCall = 0x5c6030;
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
// The plants' numbers, as profiles keep them, a std::map<std::string, int> built in: (list,
// const std::string& plant) -> number, 0 when it has none; (list, plant) -> whether it has one
// (Profile::AddPlant asks before adding); (list, number) -> the plant's name, a std::string
// through x8, empty when none; and, inside (list, PlantType handle) -> number, the map's find
// on the type's name (a std::string at sp), whose number goes to w19 after it.
constexpr Known kPlantIdOf{0x1175f18, {0xF81E0FF3, 0xA9017BFD, 0x910043FD}};
constexpr Known kPlantKnown{0x11761f0, {0xF81E0FF3, 0xA9017BFD, 0x910043FD}};
constexpr Known kPlantOfId{0x1175f5c, {0xA9BD57F6, 0xA9014FF4, 0xA9027BFD}};
constexpr Known kPlantIdOfTypeFind{0x1182d08, {0x91002260, 0x910003E1, 0x97D0A927}};
constexpr std::uint64_t kPlantIdOfTypeFound = 0x1182d24;
constexpr std::uint64_t kIsASlot = 0x20;
constexpr std::uint32_t kTargetFlags = 0xa85;  // Homing Thistle's plant food's (0x800 | 0x285)
// The checks Homing Thistle's leaf drops its target on, besides being dead.
constexpr std::uint64_t kZombieGone1 = 0x171, kZombieGone1Bit = 4, kZombieGone2 = 0x172, kZombieGone2Bit = 2;

// GameObject: its position (x towards the zombies' side). Projectile: its properties
// (resolved), who fired it, the damage it does. Plant: in plant food. Zombie: hit points.
// HomingThistleLeaf: its target.
constexpr std::uint64_t kPosX = 0x18;
constexpr std::uint64_t kProps = 0x58, kPropsHandle = 0x60, kInstigator = 0x68, kDamage = 236;
constexpr std::uint64_t kPlantFood = 296, kPlantRig = 0x118;
constexpr std::uint64_t kHitpoints = 376;
constexpr std::uint64_t kLeafTarget = 0x178;
// The lawn's right edge: the board's grid origin and size (ints, world units).
constexpr std::uint64_t kGame = 0x2593760, kBoardOfGame = 0x9a8, kGridOrigin = 0x440, kGridSize = 0x448;
constexpr float kPastEdge = 40;  // a zombie this far past the last column is on the lawn

constexpr auto kInFlight = std::chrono::milliseconds(300);  // a leaf not updated for this long is gone

class Behaviours : public std::enable_shared_from_this<Behaviours> {
public:
    Behaviours(Runtime& rt, GuestAddr base, const PlantBehaviours& b) : rt_(rt), base_(base) {
        for (const auto& k : b.kill_shots) kill_shots_.push_back({sheet(k.normal), sheet(k.special), k.animation});
        for (const auto& h : b.homing) homing_.push_back(sheet(h));
        plant_ids_ = b.plant_ids;
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
            if (std::any_of(kill_shots_.begin(), kill_shots_.end(), [](const KillShot& k) { return !k.animation.empty(); }))
                hook(rt_, base_ + kRigPlayAttackCall, "kill shot: animation", [self](CallContext& c) { self->on_attack(c.x(0), c.x(1)); });
        }
        if (!homing_.empty())
            hook(rt_, base_ + kLeafUpdate.offset, "homing retarget", [self](CallContext& c) { self->on_leaf_update(c.x(0)); });
        if (!plant_ids_.empty()) {
            hook(rt_, base_ + kPlantIdOf.offset, "plant id: of plant", [self](CallContext& c) {
                if (auto id = self->id_of(c.x(1))) return_now(c, id);
            });
            hook(rt_, base_ + kPlantKnown.offset, "plant id: known", [self](CallContext& c) {
                if (self->id_of(c.x(1))) return_now(c, 1);
            });
            hook(rt_, base_ + kPlantIdOfTypeFind.offset, "plant id: of type", [self](CallContext& c) {
                if (auto id = self->id_of(c.sp())) {
                    c.cpu().jit().SetRegister(19, id);
                    c.cpu().jit().SetPC(self->base_ + kPlantIdOfTypeFound);
                }
            });
            hook(rt_, base_ + kPlantOfId.offset, "plant id: plant of", [self](CallContext& c) { self->on_plant_of(c); });
        }
    }

private:
    // A libc++ std::string's text: short ones inline (size << 1 in the first byte), long ones
    // through a pointer (capacity | 1, size, data).
    std::string read_string(GuestAddr s) const {
        auto& mem = rt_.mem();
        std::uint8_t first = mem.read<std::uint8_t>(s);
        std::size_t size = (first & 1) ? mem.read<std::uint64_t>(s + 8) : first >> 1;
        GuestAddr data = (first & 1) ? mem.read<std::uint64_t>(s + 16) : s + 1;
        if (size > 256) return {};
        std::string out(size, '\0');
        for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<char>(mem.read<std::uint8_t>(data + i));
        return out;
    }

    // The number given for the plant named by the std::string at `name`, 0 when none.
    std::uint32_t id_of(GuestAddr name) const {
        std::string plant = read_string(name);
        for (const auto& p : plant_ids_)
            if (p.plant == plant) return p.id;
        return 0;
    }

    // Names fit the short form (22 characters), which needs no memory of the game's.
    void on_plant_of(CallContext& c) {
        auto id = static_cast<std::uint32_t>(c.x(1));
        for (const auto& p : plant_ids_) {
            if (p.id != id || p.plant.size() > 22) continue;
            GuestAddr out = c.x(8);
            auto& mem = rt_.mem();
            for (int i = 0; i < 24; i += 8) mem.write<std::uint64_t>(out + i, 0);
            mem.write<std::uint8_t>(out, static_cast<std::uint8_t>(p.plant.size() << 1));
            for (std::size_t i = 0; i < p.plant.size(); ++i) mem.write<std::uint8_t>(out + 1 + i, static_cast<std::uint8_t>(p.plant[i]));
            return return_now(c, out);
        }
    }

    // A projectile type by its alias: the handle, made once the game is up, and the properties
    // object, once the sheet is loaded.
    struct Sheet {
        std::string rtid;
        GuestAddr handle = 0;  // a guest RtWeakPtr, kept
        GuestAddr object = 0;
    };
    struct KillShot {
        Sheet normal, special;
        std::string animation;  // the armed plant's attack label, empty for its usual one
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
        std::size_t which = 0;
        for (std::size_t i = 0; i < kill_shots_.size() && !ours; ++i) {
            auto& k = kill_shots_[i];
            if (props == object(k.special)) special = ours = true;
            else if (props == object(k.normal)) ours = true;
            which = i;
        }
        if (!ours) return;
        GuestAddr plant = resolve(projectile + kInstigator);
        if (!plant) return;
        if (special && armed_.erase(plant)) log::debug("kill shot: plant {:#x} disarmed", plant);
        if (is_zombie(target) && (hitpoints(target) <= 0 || dead(target)) && armed_.emplace(plant, which).second)
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

    // A plant's rig starts its attack animation: an armed plant's kill shot with an animation of
    // its own plays that one instead (its "use_action" fires the special projectile, as the
    // usual one's would). The label is a short std::string ("attack"), so the new one is written
    // over it when it fits the short form too.
    void on_attack(GuestAddr rig, GuestAddr label) {
        if (armed_.empty() || read_string(label) != "attack") return;
        for (auto [plant, which] : armed_) {
            const std::string& animation = kill_shots_[which].animation;
            if (animation.empty() || animation.size() > 22 || rt_.mem().read<std::uint8_t>(plant + kPlantFood)) continue;
            if (resolve(plant + kPlantRig) != rig) continue;
            auto& mem = rt_.mem();
            mem.write<std::uint8_t>(label, static_cast<std::uint8_t>(animation.size() << 1));
            for (std::size_t i = 0; i < animation.size(); ++i) mem.write<std::uint8_t>(label + 1 + i, static_cast<std::uint8_t>(animation[i]));
            mem.write<std::uint8_t>(label + 1 + animation.size(), 0);
            log::debug("kill shot: plant {:#x} attacks with {}", plant, animation);
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
    std::vector<PlantBehaviours::PlantId> plant_ids_;
    GuestAddr scratch_ = 0, ref_ = 0, handle_ = 0, iter_ = 0, zombie_class_ = 0;
    std::map<GuestAddr, std::size_t> armed_;  // plants whose next shot is the special one, and whose kill shot it is
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
    auto colon = text.find(':', eq == std::string::npos ? 0 : eq);
    std::size_t end = colon == std::string::npos ? text.size() : colon;
    if (eq == std::string::npos || eq == 0 || eq + 1 >= end || colon + 1 == text.size() || (colon != std::string::npos && text.size() - colon - 1 > 22)) {
        error = "--kill-shot wants <normal projectile>=<special projectile>[:<attack animation>] (the animation up to 22 characters), got \"" + text + "\"";
        return false;
    }
    out = {text.substr(0, eq), text.substr(eq + 1, end - eq - 1), colon == std::string::npos ? std::string() : text.substr(colon + 1)};
    return true;
}

bool parse_plant_id(const std::string& text, PlantBehaviours::PlantId& out, std::string& error) {
    auto eq = text.find('=');
    std::uint32_t id = 0;
    bool number = eq != std::string::npos && eq + 1 < text.size() && text.size() - eq - 1 <= 9 &&
                  std::all_of(text.begin() + eq + 1, text.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
    if (number) id = static_cast<std::uint32_t>(std::stoul(text.substr(eq + 1)));
    if (eq == 0 || eq > 22 || !number || id == 0) {
        error = "--plant-id wants <plant>=<number> (a name of up to 22 characters, a number from 1), got \"" + text + "\"";
        return false;
    }
    out = {text.substr(0, eq), id};
    return true;
}

bool install_plant_behaviours(Runtime& rt, const Module& pvz2, const PlantBehaviours& behaviours) {
    if (behaviours.empty()) return false;
    GuestAddr base = pvz2.base();
    for (const Known* k : {&kHit, &kAddProjectile, &kLeafUpdate, &kLeafSetTarget, &kParseRtid, &kRegistry,
                           &kHandleRef, &kRefIsNull, &kRefType, &kTypeTable, &kRefResolve, &kRefFree, &kIterate, &kIterHasNext,
                           &kIterCurrent, &kIterNext, &kIterDone, &kRigPlayAttack, &kZombieDead, &kZombieClass, &kPlantIdOf, &kPlantKnown, &kPlantOfId,
                           &kPlantIdOfTypeFind})
        if (!code_matches(rt, base + k->offset, k->code)) {
            log::warn("plant behaviours (--kill-shot, --homing-retarget, --plant-id): this libPVZ2.so is a build they don't know ({:#x} differs); left off",
                      k->offset);
            return false;
        }
    std::make_shared<Behaviours>(rt, base, behaviours)->install();
    for (const auto& k : behaviours.kill_shots)
        log::info("kill shot: {} arms {}{}", k.normal, k.special, k.animation.empty() ? "" : ", fired with its " + k.animation + " animation");
    for (const auto& h : behaviours.homing) log::info("homing retarget: {}", h);
    for (const auto& p : behaviours.plant_ids) log::info("plant id: {} is {} in the save", p.plant, p.id);
    return true;
}

}  // namespace rfu
