#include "../OptiScaler/framegen/dlssg/DlssgRuntimeGeneration.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
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
// 7. UpdateInitPaths (D3D12) hands the final path list to the loader, only with the unlocker enabled.
// 8. NoteGameDlssgRuntime: only with the mod loaded and no Linux FSR fallback, the mod's dlssg_sm86 tree excluded,
//    a later list without a runtime keeps the result, and one warning only for sdli1995 0.4.1+ with generation A.

using namespace DlssgRuntimeGeneration;

static std::string ReadFile(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::fprintf(stderr, "cannot open %s\n", path);
        std::exit(1);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

static std::string Between(const std::string& text, const std::string& from, const std::string& to)
{
    const auto start = text.find(from);
    assert(start != std::string::npos);
    const auto end = text.find(to, start + from.size());
    assert(end != std::string::npos);
    return text.substr(start, end - start);
}

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

    // Test 7
    const auto dx12 = ReadFile("OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp");
    const auto initPaths = Between(dx12, "static void UpdateInitPaths(", "\n}\n");
    const auto assigned = initPaths.find("InFeatureInfo->PathListInfo.Length =");
    const auto gate = initPaths.find("if (Config::Instance()->FGDLSSGAmpereMfgUnlock.value_or_default())");
    const auto note =
        initPaths.find("AmpereMfgLoader::NoteGameDlssgRuntime(State::Instance().NVNGX_FeatureInfo_Paths);");
    assert(assigned != std::string::npos && gate != std::string::npos && note != std::string::npos);
    assert(assigned < gate && gate < note && "noted after the final list is built, only with the unlocker on");
    std::puts("Test 7 PASS: D3D12 NGX init hands the final path list to the loader");

    // Test 8
    const auto loader = ReadFile("OptiScaler/framegen/dlssg/AmpereMfgLoader.cpp");
    const auto body = Between(loader, "void NoteGameDlssgRuntime(", "\n}\n");
    const auto loadedGate = body.find("if (!s_status.DllLoaded || s_status.FsrFallbackActive)");
    const auto select = body.find("DlssgRuntimeGeneration::SelectGameRuntime(");
    assert(loadedGate != std::string::npos && select != std::string::npos && loadedGate < select);
    assert(body.find("(modDir / L\"dlssg_sm86\").wstring()") != std::string::npos);
    assert(body.find("if (runtime.empty() || runtime == s_status.GameDlssgPath)\n        return;") !=
           std::string::npos);
    const auto warn = body.find("LOG_WARN(");
    assert(warn != std::string::npos && body.find("LOG_WARN(", warn + 1) == std::string::npos);
    assert(body.find("s_status.GameDlssgGenerationA && AmpereMfgStatusText::IsSdli1995AtLeast041(s_status) && "
                     "!warned") < warn);
    assert(body.find("DLSS Swapper") != std::string::npos && body.find("XeMFG (Source=Game)") != std::string::npos);
    std::puts("Test 8 PASS: loader records the runtime and warns once, only when the mod refuses it");

    std::puts("All DLSS-G runtime generation tests passed (8/8).");
    return 0;
}
