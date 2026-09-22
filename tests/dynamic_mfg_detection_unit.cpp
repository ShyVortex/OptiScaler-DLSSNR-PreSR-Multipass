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

    std::printf("=== All Dynamic Multi-Frame Generation (DMFG) Unit Tests PASSED! ===\n");
    return 0;
}
