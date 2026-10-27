// Every literal [DlssNr] key Config.cpp reads must be documented in OptiScaler.ini, so settings merged from
// upstream cannot ship without an INI entry. Build and run from the repo root:
// g++ -std=c++20 tests/nr_ini_keys_unit.cpp && ./a.out
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#ifndef SOURCE_DIR
#define SOURCE_DIR "."
#endif

// Older keys that predate this check: compare/debug overlay settings and a legacy alias. Document one and
// remove it here; the test fails if an entry is documented or no longer read, so this list only shrinks.
static const std::set<std::string> Undocumented = { "ApplyModel",   "BeforeUpscale", "Compare",
                                                    "CompareSplit", "CompareSwap",   "CompareTags",
                                                    "CompareZoom",  "HoldFrame",     "TagScale" };

static int failures = 0;

static void Check(bool ok, const std::string& name)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
    failures += ok ? 0 : 1;
}

static std::string Read(const char* path)
{
    std::ifstream file(std::string(SOURCE_DIR) + "/" + path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

int main()
{
    std::printf("NR INI key documentation unit tests\n");
    const std::string config = Read("OptiScaler/Config.cpp");
    const std::string ini = Read("OptiScaler.ini");

    const auto begin = ini.find("[DlssNr]");
    const auto end = begin == std::string::npos ? std::string::npos : ini.find("\n[", begin + 1);
    const std::string section = begin == std::string::npos ? std::string() : ini.substr(begin, end - begin);

    std::set<std::string> read;
    const std::regex reader(R"re(read\w+\("DlssNr",\s*"(\w+)")re");
    for (auto it = std::sregex_iterator(config.begin(), config.end(), reader); it != std::sregex_iterator(); ++it)
        read.insert((*it)[1].str());

    const auto documented = [&](const std::string& key)
    { return std::regex_search(section, std::regex("(^|\\n);?\\s*" + key + "\\s*=")); };

    Check(!section.empty() && read.size() > 60, "Test 1: found the [DlssNr] section and Config.cpp's readers");

    std::string missing;
    for (const auto& key : read)
        if (!Undocumented.count(key) && !documented(key))
            missing += " " + key;
    Check(missing.empty(), "Test 2: every [DlssNr] key read by Config.cpp is documented" +
                               (missing.empty() ? std::string() : " (missing:" + missing + ")"));

    std::string stale;
    for (const auto& key : Undocumented)
        if (!read.count(key) || documented(key))
            stale += " " + key;
    Check(stale.empty(), "Test 3: the undocumented allowlist has no stale entries" +
                             (stale.empty() ? std::string() : " (stale:" + stale + ")"));

    const char* added[] = { "MatchGuides",     "RenderMotionScale",     "HistoryRejection",
                            "HaloSuppression", "HaloBrightSuppression", "HaloRadius" };
    Check(std::all_of(std::begin(added), std::end(added), [&](const char* key) { return documented(key); }) &&
              section.find("5 Edge-aware") != std::string::npos,
          "Test 4: the 6125a75 keys and Transfer 5 (Edge-aware) are documented");

    // PassProfiles applies an explicit Pass{N}Preset/Style of 0 as preset 0 / standard; only auto inherits.
    Check(section.find(";   Preset: auto = inherit pass 1, 0 = Automatic") != std::string::npos &&
              section.find(";   Style: auto = inherit pass 1, 0 = standard") != std::string::npos &&
              section.find("0 = auto (inherit pass 1)") == std::string::npos,
          "Test 5: per-pass Preset/Style comments say only auto inherits pass 1");

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
