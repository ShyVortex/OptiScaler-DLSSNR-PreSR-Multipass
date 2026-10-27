#include "../OptiScaler/framegen/dlssg/AmpereMfgLoader.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

int main()
{
    std::printf("=== Running Dynamic Multi-Frame Generation (DMFG) Detection Unit Tests ===\n");

    using namespace AmpereMfgLoader;

    // Test 1: Empty and non-existent path edge cases
    {
        assert(!HasDynamicMfgSupport("") && "Empty path must return false");
        assert(!HasDynamicMfgSupport("non_existent_file.dll") && "Non-existent file must return false");
        std::printf("  [PASS] Case 1: Empty and non-existent path handling verified\n");
    }

    // Test 2: Standard sdli1995 DLL (no DynamicMFG signature)
    {
        auto tempDir = std::filesystem::temp_directory_path() / "optiscaler_dmfg_test";
        std::filesystem::create_directories(tempDir);

        auto sdliDll = tempDir / "sdli_version.dll";
        {
            std::ofstream f(sdliDll, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            f << "This is standard sdli1995 0.3.0 binary with DLSSG_SM75_SLOTS and dlssg-310.9";
        }

        assert(!HasDynamicMfgSupport(sdliDll) && "sdli1995 binary must not report Dynamic MFG support");
        std::printf("  [PASS] Case 2: sdli1995 upstream binary correctly identified as lacking DynamicMFG\n");

        std::filesystem::remove(sdliDll);
    }

    // Test 3: SilyNoMeta fork with DynamicMFG needle
    {
        auto tempDir = std::filesystem::temp_directory_path() / "optiscaler_dmfg_test";
        std::filesystem::create_directories(tempDir);

        auto silyDll1 = tempDir / "sily_dyn_mfg.dll";
        {
            std::ofstream f(silyDll1, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            f << "Padding data here... DynamicMFG configuration and runtime hooks...";
        }

        assert(HasDynamicMfgSupport(silyDll1) && "Binary with DynamicMFG needle must be detected");
        std::printf("  [PASS] Case 3: SilyNoMeta binary detected via DynamicMFG signature\n");

        std::filesystem::remove(silyDll1);
    }

    // Test 4: SilyNoMeta fork with DynamicTargetFPS needle
    {
        auto tempDir = std::filesystem::temp_directory_path() / "optiscaler_dmfg_test";
        std::filesystem::create_directories(tempDir);

        auto silyDll2 = tempDir / "sily_dyn_target.dll";
        {
            std::ofstream f(silyDll2, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            f << "Padding data here... DynamicTargetFPS support present...";
        }

        assert(HasDynamicMfgSupport(silyDll2) && "Binary with DynamicTargetFPS needle must be detected");
        std::printf("  [PASS] Case 4: SilyNoMeta binary detected via DynamicTargetFPS signature\n");

        std::filesystem::remove(silyDll2);
    }

    // Test 5: SilyNoMeta fork with SilyNoMeta authorship needle
    {
        auto tempDir = std::filesystem::temp_directory_path() / "optiscaler_dmfg_test";
        std::filesystem::create_directories(tempDir);

        auto silyDll3 = tempDir / "sily_author.dll";
        {
            std::ofstream f(silyDll3, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            f << "Fork maintained by SilyNoMeta for SM86/SM75...";
        }

        assert(HasDynamicMfgSupport(silyDll3) && "Binary with SilyNoMeta needle must be detected");
        std::printf("  [PASS] Case 5: SilyNoMeta binary detected via author signature\n");

        std::filesystem::remove(silyDll3);
        std::filesystem::remove_all(tempDir);
    }

    // Test 5b: SilyNoMeta v0.3.5-2 DLSSG_RequestControl export signature
    {
        auto tempDir = std::filesystem::temp_directory_path() / "optiscaler_dmfg_test";
        std::filesystem::create_directories(tempDir);

        auto silyDll4 = tempDir / "sily_v0352_export.dll";
        {
            std::ofstream f(silyDll4, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            f << "Export table contains DLSSG_UniversalProxy and DLSSG_RequestControl...";
        }

        assert(HasDynamicMfgSupport(silyDll4) && "Binary with DLSSG_RequestControl needle must be detected");
        std::printf("  [PASS] Case 5b: SilyNoMeta v0.3.5-2 binary detected via DLSSG_RequestControl export\n");

        std::filesystem::remove(silyDll4);
        std::filesystem::remove_all(tempDir);
    }

    // Test 5c: SilyNoMeta v0.3.5-2 UTF-16LE DLSSG-SM86-75-COMPANION signature
    {
        auto tempDir = std::filesystem::temp_directory_path() / "optiscaler_dmfg_test";
        std::filesystem::create_directories(tempDir);

        auto silyDll5 = tempDir / "sily_v0352_utf16.dll";
        {
            std::ofstream f(silyDll5, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            // UTF-16LE 'DLSSG-SM86-75-COMPANION'
            std::u16string u16Companion = u"DLSSG-SM86-75-COMPANION";
            f.write(reinterpret_cast<const char*>(u16Companion.data()), u16Companion.size() * sizeof(char16_t));
        }

        assert(HasDynamicMfgSupport(silyDll5) && "Binary with UTF-16LE DLSSG-SM86-75-COMPANION must be detected");
        std::printf("  [PASS] Case 5c: SilyNoMeta v0.3.5-2 binary detected via UTF-16LE COMPANION string\n");

        std::filesystem::remove(silyDll5);
        std::filesystem::remove_all(tempDir);
    }

    // Test 6: Buffer chunk boundary crossing detection
    {
        auto tempDir = std::filesystem::temp_directory_path() / "optiscaler_dmfg_test";
        std::filesystem::create_directories(tempDir);

        auto splitDll = tempDir / "split_needle.dll";
        {
            std::ofstream f(splitDll, std::ios::binary);
            // Write 65530 bytes of padding so the needle crosses the 65536 byte boundary
            std::string padding(65530, 'A');
            f.write(padding.data(), padding.size());
            f << "DynamicMFG";
            std::string tail(1024, 'B');
            f.write(tail.data(), tail.size());
        }

        assert(HasDynamicMfgSupport(splitDll) && "Needle crossing 64KB chunk boundary must be detected");
        std::printf("  [PASS] Case 6: Chunk boundary overlap detection verified\n");

        std::filesystem::remove(splitDll);
        std::filesystem::remove_all(tempDir);
    }

    // Test 7: UTF-16LE needle detection (actual format in compiled MSVC binaries)
    {
        auto tempDir = std::filesystem::temp_directory_path() / "optiscaler_dmfg_test";
        std::filesystem::create_directories(tempDir);

        auto utf16Dll = tempDir / "sily_utf16.dll";
        {
            std::ofstream f(utf16Dll, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            // Write UTF-16LE DynamicTargetFPS: 'D', 0, 'y', 0, 'n', 0, ...
            const char* utf16Str = "D\0y\0n\0a\0m\0i\0c\0T\0a\0r\0g\0e\0t\0F\0P\0S\0";
            f.write(utf16Str, 32);
        }

        assert(HasDynamicMfgSupport(utf16Dll) && "Binary with UTF-16LE DynamicTargetFPS must be detected");
        std::printf("  [PASS] Case 7: UTF-16LE needle format in compiled binaries verified\n");

        std::filesystem::remove(utf16Dll);
        std::filesystem::remove_all(tempDir);
    }

    // Test 8: Immunity to companion dlssg_sm86.ini and ReShade.ini pollution
    {
        auto tempDir = std::filesystem::temp_directory_path() / "optiscaler_dmfg_test";
        std::filesystem::create_directories(tempDir);

        auto opaqueDll = tempDir / "dlssg_sm86.dll";
        {
            std::ofstream f(opaqueDll, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            f << "Opaque binary with stripped strings...";
        }

        // Without companion INI, returns false
        assert(!HasDynamicMfgSupport(opaqueDll) && "Opaque binary alone must return false");

        // Now place a companion dlssg_sm86.ini beside it with DynamicMFG=0
        auto iniPath = tempDir / "dlssg_sm86.ini";
        {
            std::ofstream f(iniPath);
            f << "[FrameGeneration]\n";
            f << "DynamicMFG=0\n";
            f << "DynamicTargetFPS=0\n";
        }

        // Must still return false because INI presence must not grant dynamic capability to an unsupported binary
        assert(!HasDynamicMfgSupport(opaqueDll) && "Opaque binary must NOT report Dynamic MFG support from INI files");

        // Place a companion ReShade.ini beside it
        auto reshadePath = tempDir / "ReShade.ini";
        {
            std::ofstream f(reshadePath);
            f << "[DLSSG-SM86-75-COMPANION]\n";
            f << "Dynamic=0\n";
        }

        assert(!HasDynamicMfgSupport(opaqueDll) &&
               "Opaque binary must NOT report Dynamic MFG support from ReShade.ini");
        std::printf("  [PASS] Case 8: Immunity to companion dlssg_sm86.ini / ReShade.ini pollution verified\n");

        std::filesystem::remove(reshadePath);
        std::filesystem::remove(iniPath);
        std::filesystem::remove(opaqueDll);
        std::filesystem::remove_all(tempDir);
    }

    // Test 9: Real binary verification (when available)
    {
        const std::filesystem::path realSilyPaths[] = { "dlssg_for_sm86/SilyNoMeta/version.dll",
                                                        "scratch/sily_test/version.dll" };
        for (const auto& realSilyDll : realSilyPaths)
        {
            if (std::filesystem::exists(realSilyDll))
            {
                assert(HasDynamicMfgSupport(realSilyDll) &&
                       "Real SilyNoMeta version.dll must be detected as supporting Dynamic MFG");
                assert(HasSm75KernelFamily(realSilyDll) &&
                       "Real SilyNoMeta version.dll must detect SM75/SM86 bridge support");

                std::string silyName;
                ModVariant silyVariant = DetectModVariant(realSilyDll, &silyName);
                assert(silyVariant == ModVariant::SilyNoMeta &&
                       "Real SilyNoMeta version.dll must detect ModVariant::SilyNoMeta");
                assert(silyName.find("SilyNoMeta") != std::string::npos && "Detected mod name must contain SilyNoMeta");

                std::printf("  [PASS] Case 9: Real SilyNoMeta release binary verified directly (%s, mod: %s)\n",
                            realSilyDll.string().c_str(), silyName.c_str());
                break;
            }
        }

        const std::filesystem::path realSdliPaths[] = { "dlssg_for_sm86/sdli1995/version.dll",
                                                        "dlssg_for_sm86/version.dll" };
        for (const auto& realSdliDll : realSdliPaths)
        {
            if (std::filesystem::exists(realSdliDll))
            {
                assert(!HasDynamicMfgSupport(realSdliDll) && "Real sdli1995 version.dll must NOT report Dynamic MFG");

                std::string sdliName;
                ModVariant sdliVariant = DetectModVariant(realSdliDll, &sdliName);
                assert(sdliVariant == ModVariant::Sdli1995 &&
                       "Real sdli1995 version.dll must detect ModVariant::Sdli1995");
                // The version comes only from the binary itself; without one the name is the family alone.
                const std::string stated = ReadEmbeddedModVersion(realSdliDll, ModVariant::Sdli1995);
                assert(sdliName == (stated.empty() ? std::string("sdli1995") : "sdli1995 v" + stated) &&
                       "Detected mod name must be the family plus the binary's own version, if any");

                std::printf("  [PASS] Case 9b: Real sdli1995 release binary verified as clean (%s, mod: %s)\n",
                            realSdliDll.string().c_str(), sdliName.c_str());

                // Verify modular 0.4.x subdirectory layout
                auto baseDir = realSdliDll.parent_path();
                if (std::filesystem::exists(baseDir / "dlssg_sm86"))
                {
                    assert(std::filesystem::exists(baseDir / "dlssg_sm86" / "backend" / "sm86_backend.dll") &&
                           "0.4.x modular layout must contain backend/sm86_backend.dll");
                    assert(std::filesystem::exists(baseDir / "dlssg_sm86" / "kernels" / "fg_kernels.dll") &&
                           "0.4.x modular layout must contain kernels/fg_kernels.dll");
                    assert(std::filesystem::exists(baseDir / "dlssg_sm86" / "runtime" / "nvngx_dlssg.dll") &&
                           "0.4.x modular layout must contain runtime/nvngx_dlssg.dll");
                    std::printf(
                        "  [PASS] Case 9c: Real sdli1995 0.4.x modular directory structure verified (%s/dlssg_sm86)\n",
                        baseDir.string().c_str());
                }
                break;
            }
        }
    }

    // Test 10: DetectModVariant synthetic verification
    {
        auto tempDir = std::filesystem::temp_directory_path() / "optiscaler_mod_test";
        std::filesystem::create_directories(tempDir);

        // 10a: Empty / nonexistent
        std::string name;
        assert(DetectModVariant("", &name) == ModVariant::Unknown && name == "Unknown");
        assert(DetectModVariant("non_existent_dll.dll", &name) == ModVariant::Unknown && name == "Unknown");

        // 10b: Synthetic SilyNoMeta v0.3.5-5
        auto silyV0355Dll = tempDir / "sily_0355.dll";
        {
            std::ofstream f(silyV0355Dll, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            f << "Control engine v0.3.5-5 integrated in the proxy; optional panel is independent.";
        }
        assert(DetectModVariant(silyV0355Dll, &name) == ModVariant::SilyNoMeta);
        assert(name == "SilyNoMeta v0.3.5-5");

        // 10c: Synthetic sdli1995 with DlssgProxy exports
        auto sdliSynthDll = tempDir / "sdli_synth.dll";
        {
            std::ofstream f(sdliSynthDll, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            f << "Export: DlssgProxy_Name and DlssgProxy_Role with standard proxy exports";
        }
        assert(DetectModVariant(sdliSynthDll, &name) == ModVariant::Sdli1995);
        assert(name == "sdli1995");

        // 10d: Synthetic sdli1995 v0.4.0 binary signatures
        auto sdli040Dll = tempDir / "sdli_040.dll";
        {
            std::ofstream f(sdli040Dll, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            f << "Auto was removed in 0.4.0 and sm86_backend.dll is next to the proxy";
        }
        assert(DetectModVariant(sdli040Dll, &name) == ModVariant::Sdli1995);
        assert(name == "sdli1995" && "no version field in the binary: family only, never a hardcoded number");
        assert(!HasDynamicMfgSupport(sdli040Dll));

        // 10e: sdli1995 0.4.0 with ReShade.ini and dlssg_sm86.ini companion files must remain sdli1995
        auto dummyIni = tempDir / "dlssg_sm86.ini";
        {
            std::ofstream f(dummyIni);
            f << "[FrameGeneration]\nDynamicMFG=0\n";
        }
        auto dummyReshade = tempDir / "ReShade.ini";
        {
            std::ofstream f(dummyReshade);
            f << "[DLSSG-SM86-75-COMPANION]\nDynamic=0\n";
        }
        assert(DetectModVariant(sdli040Dll, &name) == ModVariant::Sdli1995);
        assert(name == "sdli1995");
        assert(!HasDynamicMfgSupport(sdli040Dll));

        // 10f: sdli1995 0.4.1 states its version in the log-session metadata ("version\0" + "0.4.1")
        auto sdli041Dll = tempDir / "sdli_041.dll";
        {
            std::ofstream f(sdli041Dll, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            f << "Auto was removed in 0.4.0 and sm86_backend.dll is next to the proxy";
            f << std::string("proxy_sha256\0\0\0version\0"
                             "0.4.1\0\0\0log\0",
                             33);
        }
        assert(DetectModVariant(sdli041Dll, &name) == ModVariant::Sdli1995);
        assert(name == "sdli1995 v0.4.1");

        // 10g: a "version" field that is not a version leaves the family alone
        auto sdliBadVersionDll = tempDir / "sdli_badver.dll";
        {
            std::ofstream f(sdliBadVersionDll, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            f << "Auto was removed in 0.4.0";
            f << std::string("version\0"
                             "abc\0 version\0"
                             "4\0 version\0"
                             "0.4.\0",
                             36);
        }
        assert(DetectModVariant(sdliBadVersionDll, &name) == ModVariant::Sdli1995);
        assert(name == "sdli1995");

        // 10h: SilyNoMeta versions are parsed, not matched against a fixed list (ASCII and UTF-16LE)
        auto silyV036Dll = tempDir / "sily_036.dll";
        {
            std::ofstream f(silyV036Dll, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            f << "Control engine v0.3.6 integrated in the proxy";
        }
        assert(DetectModVariant(silyV036Dll, &name) == ModVariant::SilyNoMeta);
        assert(name == "SilyNoMeta v0.3.6");
        auto silyWideDll = tempDir / "sily_wide.dll";
        {
            std::ofstream f(silyWideDll, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            for (char c : std::string("Control engine v0.3.7-2 ready"))
            {
                f.put(c);
                f.put('\0');
            }
        }
        assert(DetectModVariant(silyWideDll, &name) == ModVariant::SilyNoMeta);
        assert(name == "SilyNoMeta v0.3.7-2");

        // 10i: SilyNoMeta markers without a stated version give the family alone
        auto silyNoVersionDll = tempDir / "sily_nover.dll";
        {
            std::ofstream f(silyNoVersionDll, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            f << "Exports DLSSG_RequestControl and DLSSG_GetControlStatus";
        }
        assert(DetectModVariant(silyNoVersionDll, &name) == ModVariant::SilyNoMeta);
        assert(name == "SilyNoMeta");

        // 10j: no versioned mod name is hardcoded in the loader
        {
            std::ifstream header("OptiScaler/framegen/dlssg/AmpereMfgLoader.h", std::ios::binary);
            assert(header.is_open() && "run from the repository root");
            const std::string text((std::istreambuf_iterator<char>(header)), std::istreambuf_iterator<char>());
            assert(text.find("\"sdli1995 v") == std::string::npos && text.find("\"SilyNoMeta v") == std::string::npos &&
                   "mod versions must be read from the binary, never written as a fallback");
        }
        std::printf("  [PASS] Case 10f-10j: versions read from the binary; family name alone when none is stated\n");

        std::filesystem::remove(sdli041Dll);
        std::filesystem::remove(sdliBadVersionDll);
        std::filesystem::remove(silyV036Dll);
        std::filesystem::remove(silyWideDll);
        std::filesystem::remove(silyNoVersionDll);
        std::filesystem::remove(dummyReshade);
        std::filesystem::remove(dummyIni);
        std::filesystem::remove(sdli040Dll);
        std::filesystem::remove(silyV0355Dll);
        std::filesystem::remove(sdliSynthDll);
        std::filesystem::remove_all(tempDir);

        std::printf("  [PASS] Case 10: DetectModVariant synthetic unit tests passed\n");
    }

    std::printf("=== All Dynamic Multi-Frame Generation (DMFG) Unit Tests PASSED! ===\n");
    return 0;
}
