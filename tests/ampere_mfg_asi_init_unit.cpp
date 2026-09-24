#include "../OptiScaler/framegen/dlssg/AmpereMfgLoader.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

int main()
{
    std::printf("=== Running Ampere MFG ASI Initialization & Export Unit Tests ===\n");

    using namespace AmpereMfgLoader;

    // Case 1: Empty and non-existent path edge cases
    {
        assert(!HasAsiInitExport("") && "Empty path must return false");
        assert(!HasAsiInitExport("non_existent_file.dll") && "Non-existent file must return false");
        std::printf("  [PASS] Case 1: Empty and non-existent path handling verified\n");
    }

    // Case 2: Status struct defaults
    {
        Status status;
        assert(!status.AsiInitInvoked && "AsiInitInvoked must default to false");
        std::printf("  [PASS] Case 2: Status struct AsiInitInvoked default verified\n");
    }

    // Case 3: Synthetic binary without InitializeASI
    {
        auto tempDir = std::filesystem::temp_directory_path() / "optiscaler_asi_test";
        std::filesystem::create_directories(tempDir);

        auto testDll = tempDir / "no_asi.dll";
        {
            std::ofstream f(testDll, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            f << "Standard export table with GetFileVersionInfoA, DLSSG_SM75_SLOTS, etc.";
        }

        assert(!HasAsiInitExport(testDll) && "Binary without InitializeASI must return false");
        std::printf("  [PASS] Case 3: Binary without InitializeASI correctly identified\n");

        std::filesystem::remove(testDll);
    }

    // Case 4: Synthetic binary with InitializeASI signature
    {
        auto tempDir = std::filesystem::temp_directory_path() / "optiscaler_asi_test";
        std::filesystem::create_directories(tempDir);

        auto testDll = tempDir / "with_asi.dll";
        {
            std::ofstream f(testDll, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            f << "Some prefix data... Export: InitializeASI ... Some suffix data";
        }

        assert(HasAsiInitExport(testDll) && "Binary with InitializeASI must return true");
        std::printf("  [PASS] Case 4: Binary with InitializeASI correctly detected\n");

        std::filesystem::remove(testDll);
    }

    // Case 4b: Synthetic binary with DLSSG_UniversalProxy signature (v0.3.5-2)
    {
        auto tempDir = std::filesystem::temp_directory_path() / "optiscaler_asi_test";
        std::filesystem::create_directories(tempDir);

        auto testDll = tempDir / "with_proxy.dll";
        {
            std::ofstream f(testDll, std::ios::binary);
            f << "MZ\x90\x00\x03\x00\x00\x00";
            f << "Export table with DLSSG_UniversalProxy, DLSSG_RequestControl, etc.";
        }

        assert(HasAsiInitExport(testDll) && "Binary with DLSSG_UniversalProxy must return true");
        std::printf("  [PASS] Case 4b: Binary with DLSSG_UniversalProxy correctly detected\n");

        std::filesystem::remove(testDll);
    }

    // Case 5: Chunk boundary overlap detection
    {
        auto tempDir = std::filesystem::temp_directory_path() / "optiscaler_asi_test";
        std::filesystem::create_directories(tempDir);

        auto testDll = tempDir / "boundary_asi.dll";
        {
            std::ofstream f(testDll, std::ios::binary);
            constexpr size_t paddingSize = 65536 - 6; // 'InitializeASI' splits across chunk boundary
            std::string padding(paddingSize, 'X');
            f.write(padding.data(), padding.size());
            f << "InitializeASI";
            f << "More data here...";
        }

        assert(HasAsiInitExport(testDll) && "Binary with InitializeASI crossing chunk boundary must be detected");
        std::printf("  [PASS] Case 5: Chunk boundary overlap detection verified\n");

        std::filesystem::remove(testDll);
        std::filesystem::remove(tempDir);
    }

    // Case 6: Real repository binaries
    {
        std::filesystem::path realSily = "dlssg_for_sm86/SilyNoMeta/version.dll";
        if (std::filesystem::exists(realSily))
        {
            assert(HasAsiInitExport(realSily) &&
                   "Real SilyNoMeta binary must contain InitializeASI or DLSSG_UniversalProxy export");
            assert(HasDynamicMfgSupport(realSily) && "Real SilyNoMeta binary must contain DynamicMFG support");
            std::printf("  [PASS] Case 6a: Real SilyNoMeta release binary verified (has proxy export & DynamicMFG)\n");
        }
        else
        {
            std::printf("  [SKIP] Case 6a: Real SilyNoMeta binary not found at %s\n", realSily.string().c_str());
        }

        std::filesystem::path realSdli = "dlssg_for_sm86/sdli1995/version.dll";
        if (std::filesystem::exists(realSdli))
        {
            assert(!HasAsiInitExport(realSdli) && "Real sdli1995 binary must NOT contain InitializeASI export");
            assert(!HasDynamicMfgSupport(realSdli) && "Real sdli1995 binary must NOT contain DynamicMFG support");
            std::printf("  [PASS] Case 6b: Real sdli1995 release binary verified (no InitializeASI, static only)\n");
        }
        else
        {
            std::printf("  [SKIP] Case 6b: Real sdli1995 binary not found at %s\n", realSdli.string().c_str());
        }
    }

    std::printf("=== All Ampere MFG ASI Initialization & Export Unit Tests PASSED! ===\n");
    return 0;
}
