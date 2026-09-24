#include "../OptiScaler/framegen/dlssg/AmpereMfgLoader.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

    // Test 8: Companion dlssg_sm86.ini detection fallback
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

        // Now place SilyNoMeta's companion dlssg_sm86.ini beside it
        auto iniPath = tempDir / "dlssg_sm86.ini";
        {
            std::ofstream f(iniPath);
            f << "[FrameGeneration]\n";
            f << "DynamicMFG=0\n";
            f << "DynamicTargetFPS=0\n";
        }

        assert(HasDynamicMfgSupport(opaqueDll) && "Companion dlssg_sm86.ini with DynamicMFG must be recognized");
        std::printf("  [PASS] Case 8: Companion dlssg_sm86.ini detection fallback verified\n");

        std::filesystem::remove(iniPath);
        std::filesystem::remove(opaqueDll);
        std::filesystem::remove_all(tempDir);
    }

    // Test 9: Real binary verification (when available)
    {
        const std::filesystem::path realSilyPaths[] = {
            "dlssg_for_sm86/SilyNoMeta/version.dll",
            "scratch/sily_test/version.dll"
        };
        for (const auto& realSilyDll : realSilyPaths)
        {
            if (std::filesystem::exists(realSilyDll))
            {
                assert(HasDynamicMfgSupport(realSilyDll) && "Real SilyNoMeta version.dll must be detected as supporting Dynamic MFG");
                assert(HasSm75KernelFamily(realSilyDll) && "Real SilyNoMeta version.dll must detect SM75/SM86 bridge support");
                std::printf("  [PASS] Case 9: Real SilyNoMeta release binary verified directly (%s)\n", realSilyDll.string().c_str());
                break;
            }
        }

        const std::filesystem::path realSdliPaths[] = {
            "dlssg_for_sm86/sdli1995/version.dll",
            "dlssg_for_sm86/version.dll"
        };
        for (const auto& realSdliDll : realSdliPaths)
        {
            if (std::filesystem::exists(realSdliDll))
            {
                assert(!HasDynamicMfgSupport(realSdliDll) && "Real sdli1995 version.dll must NOT report Dynamic MFG");
                std::printf("  [PASS] Case 9b: Real sdli1995 release binary verified as clean (%s)\n", realSdliDll.string().c_str());
                break;
            }
        }
    }

    std::printf("=== All Dynamic Multi-Frame Generation (DMFG) Unit Tests PASSED! ===\n");
    return 0;
}
