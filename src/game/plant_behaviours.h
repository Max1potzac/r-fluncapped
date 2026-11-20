#pragma once

// Plant behaviours the game's own classes don't have, for new plants that mods add from
// property sheets alone (tools/launcher/mods/Pinecone). Each names projectiles by their alias
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
// Like the other fixes, the addresses are those of the libPVZ2 build Reflourished 1.4.2 and
// AltverZ 1.8.3 share, checked first; on another build the behaviours stay off.

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

    bool empty() const { return kill_shots.empty() && homing.empty(); }
};

// "<normal>=<special>" for --kill-shot; false, with `error` set, when it isn't one.
bool parse_kill_shot(const std::string& text, PlantBehaviours::KillShot& out, std::string& error);

bool install_plant_behaviours(Runtime& rt, const Module& pvz2, const PlantBehaviours& behaviours);

}  // namespace rfu
