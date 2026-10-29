#include "../OptiScaler/framegen/dlssg/DlssgRuntimeGeneration.h"

#include <cassert>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

// Remnant 2 ships nvngx_dlssg.dll 3.7.10 in its Streamline folder. sdli1995 0.4.1 forwards such a pre-310 runtime
// unchanged and NVIDIA's runtime refuses RTX 20/30, so frame generation never runs (DLSS Unlocked #61). OptiScaler
// must find the file NGX loads, ignoring the unlocker's own 310.9 runtime, and classify it.
//
// Validates:
// 1. Classify: 1.x - 3.8.x are generation A, 310.x and newer generation B, an unread version unknown.
// 2. FormatVersion prints major.minor.patch.
// 3. NormalizePath / IsUnder: case, mixed separators, trailing separators, no partial-name match.
// 4. SelectGameRuntime follows search order and skips empty entries.
// 5. The unlocker's runtime is skipped; with no game copy the result is empty.
// 6. The Remnant 2 path list from the #61 log selects the Streamline copy.

using namespace DlssgRuntimeGeneration;

static std::function<bool(const std::wstring&)> Files(std::set<std::wstring> existing)
{
    return [existing](const std::wstring& path) { return existing.count(NormalizePath(path)) != 0; };
}

int main()
{
    // Test 1
    assert(Classify(1) == Generation::A);
    assert(Classify(3) == Generation::A);
    assert(Classify(309) == Generation::A);
    assert(Classify(310) == Generation::B);
    assert(Classify(311) == Generation::B);
    assert(Classify(0) == Generation::Unknown);
    std::puts("Test 1 PASS: generation A below 310, B from 310, unknown when unread");

    // Test 2
    assert(FormatVersion(3, 7, 10) == "3.7.10");
    assert(FormatVersion(310, 9, 16) == "310.9.16");
    std::puts("Test 2 PASS: version text");

    // Test 3
    assert(NormalizePath(L"C:/Games/X/") == L"c:\\games\\x");
    assert(IsUnder(L"C:\\Games\\X\\OptiScaler\\dlssg_sm86\\runtime", L"c:/games/x/optiscaler/"));
    assert(IsUnder(L"C:\\Games\\X\\OptiScaler", L"C:\\Games\\X\\OptiScaler"));
    assert(!IsUnder(L"C:\\Games\\X\\OptiScalerOld", L"C:\\Games\\X\\OptiScaler"));
    assert(!IsUnder(L"C:\\Games\\X", L"C:\\Games\\X\\OptiScaler"));
    assert(!IsUnder(L"C:\\Games\\X", L""));
    std::puts("Test 3 PASS: path comparison ignores case and separators, never matches part of a name");

    // Test 4
    const std::vector<std::wstring> ordered = { L"", L"C:\\A", L"C:/B/", L"C:\\C" };
    assert(SelectGameRuntime(ordered, {}, Files({ L"c:\\b\\nvngx_dlssg.dll", L"c:\\c\\nvngx_dlssg.dll" })) ==
           L"C:/B/nvngx_dlssg.dll");
    assert(SelectGameRuntime(ordered, {}, Files({ L"c:\\c\\nvngx_dlssg.dll" })) == L"C:\\C\\nvngx_dlssg.dll");
    std::puts("Test 4 PASS: first copy in search order, empty entries skipped");

    // Test 5
    const std::vector<std::wstring> modOnly = { L"C:\\G\\OptiScaler", L"C:\\G\\OptiScaler\\dlssg_sm86\\runtime" };
    const std::vector<std::wstring> modRoot = { L"C:\\G\\OptiScaler\\dlssg_sm86" };
    assert(SelectGameRuntime(modOnly, modRoot, Files({ L"c:\\g\\optiscaler\\dlssg_sm86\\runtime\\nvngx_dlssg.dll" }))
               .empty());
    assert(SelectGameRuntime({}, modRoot, Files({})).empty());
    std::puts("Test 5 PASS: the unlocker's own runtime is never taken for the game's");

    // Test 6
    const std::wstring root = L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Remnant2\\Remnant2";
    const std::vector<std::wstring> remnant = {
        root + L"\\Binaries\\Win64\\OptiScaler",
        L"C:/Program Files (x86)/Steam/steamapps/common/Remnant2/Remnant2/Plugins/Shared/Streamline/Binaries/"
        L"ThirdParty/Win64",
        L"C:/Program Files (x86)/Steam/steamapps/common/Remnant2/Remnant2/Binaries/ThirdParty/NVIDIA/NGX/Win64",
        root + L"\\Binaries\\Win64\\",
        root + L"\\Plugins\\Shared\\DLSS\\Binaries\\ThirdParty\\Win64",
        root + L"\\Binaries\\Win64\\OptiScaler\\dlssg_sm86\\dlssg_sm86\\runtime",
    };
    const auto files = Files({
        NormalizePath(root + L"\\Plugins\\Shared\\Streamline\\Binaries\\ThirdParty\\Win64\\nvngx_dlssg.dll"),
        NormalizePath(root + L"\\Binaries\\Win64\\OptiScaler\\dlssg_sm86\\dlssg_sm86\\runtime\\nvngx_dlssg.dll"),
    });
    const auto selected = SelectGameRuntime(remnant, { root + L"\\Binaries\\Win64\\OptiScaler\\dlssg_sm86" }, files);
    assert(NormalizePath(selected) ==
           NormalizePath(root + L"\\Plugins\\Shared\\Streamline\\Binaries\\ThirdParty\\Win64\\nvngx_dlssg.dll"));
    std::puts("Test 6 PASS: Remnant 2 path list selects the Streamline copy");

    std::puts("All DLSS-G runtime generation tests passed (6/6).");
    return 0;
}
