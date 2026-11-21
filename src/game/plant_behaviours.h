#pragma once

// Plant behaviours the game's own classes don't have, for new plants that mods add from
// property sheets alone (the Pinecone sample mod). Each names projectiles by their alias
// in PROJECTILETYPES, so a mod turns one on for its own plant through mod.json's "rfu" list.
//
//   --kill-shot <normal>=<special>
//       A plant whose <normal> projectile kills a zombie fires its next shot as <special>
//       instead, and keeps doing so until a <special> one hits something (a zombie, its shield,
//       a grid item in the way). A kill by a <special> one arms it again. It only changes what
//       the plant fires, not when: it still shoots only at a zombie in reach. Not in plant food.
//
//   --homing-retarget <projectile>
//       A homing projectile (a HomingThistleLeaf) of this kind with no target, or whose target
//       died, goes for the zombie nearest the house that the others in flight aren't already
//       enough to finish: each counts its damage against the zombie it chases.
//
//   --plant-id <plant>=<number>
//       The number the save knows a new plant by. The player's profile keeps the plants they
//       have as numbers from a list built into the game (RFLib adds its own plants to it), so a
//       plant that's only in PLANTTYPES has none: bought in the shop it's there until the game
//       is closed, then for sale again. This gives it one; pick one neither list uses (the game
//       and RFLib use 1 to 331), and keep it, since saves record plants by it.
//
// Like the other fixes, the addresses are those of the libPVZ2 build Reflourished 1.4.2 and
// AltverZ 1.8.3 share, checked first; on another build the behaviours stay off.

#include <cstdint>
#include <string>
#include <vector>

namespace rfu {

class Runtime;
class Module;

struct PlantBehaviours {
    struct KillShot {
        std::string normal, special;
    };
    std::vector<KillShot> kill_shots;
    std::vector<std::string> homing;
    struct PlantId {
        std::string plant;
        std::uint32_t id = 0;
    };
    std::vector<PlantId> plant_ids;

    bool empty() const { return kill_shots.empty() && homing.empty() && plant_ids.empty(); }
};

// "<normal>=<special>" for --kill-shot; false, with `error` set, when it isn't one.
bool parse_kill_shot(const std::string& text, PlantBehaviours::KillShot& out, std::string& error);
// "<plant>=<number>" for --plant-id, the number 1 or more.
bool parse_plant_id(const std::string& text, PlantBehaviours::PlantId& out, std::string& error);

bool install_plant_behaviours(Runtime& rt, const Module& pvz2, const PlantBehaviours& behaviours);

}  // namespace rfu
