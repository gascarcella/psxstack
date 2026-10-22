// The game's Steam artwork, embedded at build time (launcher/CMakeLists.txt: game.json launcher.steam_art).
#include "steam.h"

#include "steam_art.h"

namespace psxstack {

std::vector<SteamArt> steam_art_embedded() {
    std::vector<SteamArt> art;
    for (const auto &a : STEAM_ART) {
        if (a.suffix != nullptr) {
            art.push_back({ a.suffix, std::string(reinterpret_cast<const char *>(a.data), a.size) });
        }
    }
    return art;
}

} // namespace psxstack
