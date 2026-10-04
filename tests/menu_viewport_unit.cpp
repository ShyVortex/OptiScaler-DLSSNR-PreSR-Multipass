#include <imgui/imgui.h>
#include <imgui/imgui_internal.h>
#ifndef MENU_VIEWPORT_LEGACY
#include <menu/menu_viewport.h>
#endif

#include <cmath>
#include <cstdio>

static int failures = 0;

static void Check(bool passed, const char* name)
{
    std::printf("%s: %s\n", passed ? "PASS" : "FAIL", name);
    if (!passed)
        ++failures;
}

static void Frame(ImVec2 clientSize, ImVec2 framebufferSize, float contentHeight, bool setPosition = false,
                  ImVec2 position = { 60.0f, 60.0f }, ImVec2 workOrigin = { 0.0f, 0.0f })
{
    auto& io = ImGui::GetIO();
    io.DisplaySize = clientSize;
#ifndef MENU_VIEWPORT_LEGACY
    MenuViewport::SetFramebufferScale(framebufferSize);
#else
    (void) framebufferSize;
#endif
    ImGui::NewFrame();
    auto* viewport = ImGui::GetMainViewport();
    viewport->Pos = workOrigin;
    viewport->WorkPos = workOrigin;
    viewport->WorkSize = clientSize;
    if (setPosition)
    {
        if (auto* window = ImGui::FindWindowByName("OptiScaler viewport test"))
            ImGui::SetWindowPos(window, position, ImGuiCond_Always);
        else
            ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    }
#ifndef MENU_VIEWPORT_LEGACY
    MenuViewport::ConstrainWindow("OptiScaler viewport test");
#endif
    ImGui::Begin("OptiScaler viewport test", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse);
    ImGui::Dummy({ 700.0f, contentHeight });
    ImGui::Button("Save settings");
    ImGui::End();
    ImGui::Render();
}

static bool InsideViewport()
{
    const auto* window = ImGui::FindWindowByName("OptiScaler viewport test");
    const auto* viewport = ImGui::GetMainViewport();
    return window->Pos.x >= viewport->WorkPos.x && window->Pos.y >= viewport->WorkPos.y &&
           window->Pos.x + window->Size.x <= viewport->WorkPos.x + viewport->WorkSize.x + 1.0f &&
           window->Pos.y + window->Size.y <= viewport->WorkPos.y + viewport->WorkSize.y + 1.0f;
}

static void RunFrames(ImVec2 client, ImVec2 framebuffer, float contentHeight)
{
    for (int i = 0; i < 4; ++i)
        Frame(client, framebuffer, contentHeight);
}

int main()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    ImGui::GetPlatformIO().Renderer_TextureMaxWidth = 4096;
    ImGui::GetPlatformIO().Renderer_TextureMaxHeight = 4096;
    io.Fonts->AddFontDefault();

    RunFrames({ 5120, 1440 }, { 5120, 1440 }, 1200);
    Check(InsideViewport(), "native 5120x1440 layout fits");

    RunFrames({ 5120, 1440 }, { 3840, 1080 }, 1200);
    const auto* draw = ImGui::GetDrawData();
    Check(std::fabs(draw->DisplaySize.x * draw->FramebufferScale.x - 3840.0f) < 0.1f &&
              std::fabs(draw->DisplaySize.y * draw->FramebufferScale.y - 1080.0f) < 0.1f,
          "5120 client area maps onto 3840x1080 backbuffer");
    const auto* window = ImGui::FindWindowByName("OptiScaler viewport test");
    Check((window->Pos.y + window->Size.y) * draw->FramebufferScale.y <= 1080.0f,
          "menu bottom stays inside reduced-resolution backbuffer");

    RunFrames({ 3840, 1080 }, { 3840, 1080 }, 2200);
    Check(InsideViewport(), "expanded ultrawide NR menu remains on-screen");
    Check(ImGui::FindWindowByName("OptiScaler viewport test")->ScrollMax.y > 0.0f,
          "overflowing controls remain scrollable");

    RunFrames({ 1920, 1080 }, { 1920, 1080 }, 2200);
    Check(InsideViewport(), "expanded 16:9 menu also remains on-screen");

    Frame({ 1920, 1080 }, { 1920, 1080 }, 300, true, { 1500, 1000 });
    RunFrames({ 1920, 1080 }, { 1920, 1080 }, 300);
    Check(InsideViewport(), "dragged menu is fully recovered from screen edge");

    RunFrames({ 640, 480 }, { 640, 480 }, 2200);
    Check(InsideViewport(), "resolution reduction recovers menu bounds");

    ImGui::GetStyle() = ImGuiStyle();
    ImGui::GetStyle().ScaleAllSizes(2.0f);
    RunFrames({ 1920, 1080 }, { 1920, 1080 }, 2200);
    Check(InsideViewport(), "large manual style still has bounded scrollable window");

    Frame({ 1920, 1080 }, { 1920, 1080 }, 2200, true, { -800, -800 }, { 100, 50 });
    for (int i = 0; i < 3; ++i)
        Frame({ 1920, 1080 }, { 1920, 1080 }, 2200, false, {}, { 100, 50 });
    Check(InsideViewport(), "nonzero viewport origin is respected");

#ifndef MENU_VIEWPORT_LEGACY
    io.DisplaySize = { 5120, 1440 };
    io.MousePos = { 2500, 700 };
    MenuViewport::SetFramebufferScale({ 3840, 1080 });
    Check(io.MousePos.x == 2500 && io.MousePos.y == 700, "framebuffer scaling preserves logical mouse coordinates");
    MenuViewport::SetFramebufferScale({ 0, 0 });
    Check(io.DisplayFramebufferScale.x == 1.0f && io.DisplayFramebufferScale.y == 1.0f,
          "unknown framebuffer resets scaling instead of retaining stale ratio");
    io.DisplaySize = { 0, 0 };
    MenuViewport::SetFramebufferScale({ 3840, 1080 });
    Check(io.DisplayFramebufferScale.x == 1.0f && io.DisplayFramebufferScale.y == 1.0f,
          "minimized client area does not produce invalid scale");
    auto* viewport = ImGui::GetMainViewport();
    viewport->WorkPos = { 0, 0 };
    viewport->WorkSize = { 1, 1 };
    MenuViewport::ConstrainWindow("OptiScaler viewport test");
    const auto constraints = ImGui::GetCurrentContext()->NextWindowData.SizeConstraintRect;
    Check(constraints.Max.x >= 1.0f && constraints.Max.y >= 1.0f,
          "tiny positive viewport retains valid size constraint bounds");
#endif

    ImGui::DestroyContext();
    std::printf("Menu viewport failures: %d\n", failures);
    return failures ? 1 : 0;
}
