#include "../OptiScaler/misc/GameRootSearch.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

using GameRootSearch::FindEngineRoot;

static auto DirsWith(std::set<std::string> dirs)
{
    return [dirs](const std::filesystem::path& path) { return dirs.count(path.generic_string()) != 0; };
}

static std::string Squash(const std::string& text)
{
    std::string out;
    for (char c : text)
        if (c != ' ' && c != '\n' && c != '\r' && c != '\t')
            out += c;
    return out;
}

static std::string ReadSource(const char* path)
{
    std::ifstream file(path);
    assert(file && "source file not found; run from the repository root");
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

int main()
{
    // 1. Phantom Blade Zero: exe folder PBZ/platform/steam/ with PBZ/Engine
    {
        auto root = FindEngineRoot("/games/PBZ/platform/steam/", DirsWith({ "/games/PBZ/Engine" }));
        assert(root.has_value() && root->generic_string() == "/games/PBZ");
        std::cout << "PASS 1: exe outside Binaries finds the Engine root\n";
    }

    // 2. Same game, searched from the OptiScaler folder (3 levels up)
    {
        auto root = FindEngineRoot("/games/PBZ/platform/steam/OptiScaler", DirsWith({ "/games/PBZ/Engine" }));
        assert(root.has_value() && root->generic_string() == "/games/PBZ");
        std::cout << "PASS 2: OptiScaler folder reaches the same root\n";
    }

    // 3. No Engine directory anywhere
    {
        assert(!FindEngineRoot("/games/Other/bin/x64/", DirsWith({})).has_value());
        std::cout << "PASS 3: no Engine directory, no root\n";
    }

    // 4. Engine 4 levels up is beyond the limit
    {
        assert(!FindEngineRoot("/games/G/a/b/c/d/", DirsWith({ "/games/G/Engine" })).has_value());
        assert(FindEngineRoot("/games/G/a/b/c/d/", DirsWith({ "/games/G/Engine" }), 4).has_value());
        std::cout << "PASS 4: level limit respected\n";
    }

    // 5. Stops at the filesystem root without looping
    {
        assert(!FindEngineRoot("/a/", DirsWith({}), 10).has_value());
        assert(!FindEngineRoot("/", DirsWith({}), 10).has_value());
        std::cout << "PASS 5: stops at the root\n";
    }

    // 6. Nearest Engine ancestor wins
    {
        auto root = FindEngineRoot("/games/PBZ/platform/steam/", DirsWith({ "/games/PBZ/Engine", "/games/Engine" }));
        assert(root.has_value() && root->generic_string() == "/games/PBZ");
        std::cout << "PASS 6: nearest Engine ancestor wins\n";
    }

    // 7. startDir itself is skipped (step 2 already searched below it)
    {
        assert(!FindEngineRoot("/games/PBZ/", DirsWith({ "/games/PBZ/Engine" }), 0).has_value());
        auto root = FindEngineRoot("/games/PBZ/", DirsWith({ "/games/PBZ/Engine" }));
        assert(!root.has_value() || root->generic_string() != "/games/PBZ");
        std::cout << "PASS 7: startDir is not its own Engine root\n";
    }

    // 8. Util::FindFilePath wiring
    {
        const auto src = Squash(ReadSource("OptiScaler/Util.cpp"));
        const auto bfs = src.find("//2)Breadth-Firstsearch");
        const auto ue = src.find("for(constchar*folder:{\"Win64\",\"WinGDK\",\"Win64MasterMasterSteamPGO\"})");
        const auto engine = src.find("GameRootSearch::FindEngineRoot(startDir,isDirectory)");
        assert(bfs != std::string::npos && ue != std::string::npos && engine != std::string::npos);
        assert(bfs < ue && ue < engine);
        assert(src.find("ueFallbackSearched=true;break;") != std::string::npos);
        assert(src.find("if(!ueFallbackSearched)") != std::string::npos);
        assert(src.find("SearchDirectoryBFS(engineRoot.value())") != std::string::npos);
        std::cout << "PASS 8: Engine fallback runs last, only without the Win64 fallback, via the filtered BFS\n";
    }

    std::cout << "All game_root_search tests passed\n";
    return 0;
}
