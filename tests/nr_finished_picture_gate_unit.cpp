// The finished-picture menu gate: under external frame generation or DLSS-G the runtime skips finished-picture NR
// (674da5df), so the menu must say so, never let the option be turned on, and always let it be turned off.
// Build and run from the repo root: g++ -std=c++20 tests/nr_finished_picture_gate_unit.cpp && ./a.out
#include "../OptiScaler/dlssnr/DlssNr_Placement.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#ifndef SOURCE_DIR
#define SOURCE_DIR "."
#endif

static int failures = 0;

static void Check(bool ok, const char* name)
{
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", name);
    failures += ok ? 0 : 1;
}

static std::string Read(const char* path)
{
    std::ifstream file(std::string(SOURCE_DIR) + "/" + path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

static int Count(const std::string& text, const std::string& what)
{
    int n = 0;
    for (auto at = text.find(what); at != std::string::npos; at = text.find(what, at + 1))
        ++n;
    return n;
}

int main()
{
    std::printf("NR finished-picture gate unit tests\n");
    using DlssNr::ResolveFinishedPictureGate;

    {
        constexpr auto off = ResolveFinishedPictureGate(false, false, false);
        constexpr auto on = ResolveFinishedPictureGate(true, false, false);
        Check(!off.blocked && off.canToggle && !off.notApplied && !on.blocked && on.canToggle && !on.notApplied,
              "Test 1: without frame generation the option behaves as before");
    }
    {
        constexpr auto external = ResolveFinishedPictureGate(false, true, false);
        constexpr auto dlssg = ResolveFinishedPictureGate(false, false, true);
        Check(external.blocked && !external.canToggle && !external.notApplied && dlssg.blocked && !dlssg.canToggle,
              "Test 2: external FG or DLSS-G blocks turning the option on");
    }
    {
        constexpr auto stuck = ResolveFinishedPictureGate(true, true, false);
        Check(stuck.blocked && stuck.canToggle && stuck.notApplied,
              "Test 3: an option already on stays toggleable and is reported as not applied");
    }

    const std::string menu = Read("OptiScaler/dlssnr/DlssNr_Menu.cpp");
    const auto checkbox = menu.find("\"Apply NR to the finished picture\"");
    const auto disable = menu.find("ImGui::BeginDisabled(!finishedGate.canToggle);");
    Check(disable != std::string::npos && disable < checkbox &&
              menu.find("ImGui::TextWrapped(\"%s\", FinishedPictureBlockedWarning);") != std::string::npos &&
              menu.find("ImGui::TextWrapped(\"%s\", FinishedPictureNotAppliedWarning);") != std::string::npos &&
              menu.find("ImGuiHoveredFlags_AllowWhenDisabled))\n            ImGui::SetTooltip(finishedGate.blocked") !=
                  std::string::npos,
          "Test 4: the menu disables the checkbox and shows the warning and disabled tooltip");
    Check(menu.find(".notApplied;") != std::string::npos &&
              menu.find("not applied: finished picture blocked by frame generation") != std::string::npos,
          "Test 5: the Running status line says when the edit is discarded");

    // The menu gate mirrors the runtime: every finished-picture entry point still skips under FG.
    const std::string dx12 = Read("OptiScaler/shaders/dlssnr/DlssNr_Dx12.cpp");
    const std::string compose = Read("OptiScaler/shaders/dlssnr/DlssNr_Dx12_FinishedCompose.cpp");
    const std::string swapchain = Read("OptiScaler/wrapped/wrapped_swapchain.cpp");
    const std::string gate = "externalFrameGeneration ||";
    Check(Count(dx12, gate) == 3 && Count(compose, gate) == 1 &&
              swapchain.find("externalFgActive") != std::string::npos,
          "Test 6: the runtime finished-picture gates are still in place");

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
