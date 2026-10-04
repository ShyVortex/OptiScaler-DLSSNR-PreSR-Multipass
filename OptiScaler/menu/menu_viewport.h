#pragma once

#include <imgui/imgui_internal.h>
#include <cmath>

namespace MenuViewport
{
inline void SetFramebufferScale(ImVec2 framebufferSize)
{
    auto& io = ImGui::GetIO();
    io.DisplayFramebufferScale = { 1.0f, 1.0f };
    if (framebufferSize.x > 0.0f && framebufferSize.y > 0.0f && io.DisplaySize.x > 0.0f && io.DisplaySize.y > 0.0f &&
        std::isfinite(framebufferSize.x) && std::isfinite(framebufferSize.y) && std::isfinite(io.DisplaySize.x) &&
        std::isfinite(io.DisplaySize.y))
    {
        // Keep layout and mouse coordinates in client-area units. The renderer
        // maps those units onto the actual target, including fullscreen downscaling.
        io.DisplayFramebufferScale = { framebufferSize.x / io.DisplaySize.x, framebufferSize.y / io.DisplaySize.y };
    }
}

inline void ConstrainWindow(const char* title)
{
    const auto* viewport = ImGui::GetMainViewport();
    if (viewport->WorkSize.x <= 0.0f || viewport->WorkSize.y <= 0.0f)
        return;

    const auto& style = ImGui::GetStyle();
    const ImVec2 padding { ImMax(style.DisplayWindowPadding.x, style.DisplaySafeAreaPadding.x),
                           ImMax(style.DisplayWindowPadding.y, style.DisplaySafeAreaPadding.y) };
    const ImVec2 minimum { viewport->WorkPos.x + padding.x, viewport->WorkPos.y + padding.y };
    const ImVec2 maximum { viewport->WorkPos.x + viewport->WorkSize.x - padding.x,
                           viewport->WorkPos.y + viewport->WorkSize.y - padding.y };
    const ImVec2 available { ImMax(1.0f, maximum.x - minimum.x), ImMax(1.0f, maximum.y - minimum.y) };
    // CalcWindowNextAutoFitSize below consumes these constraints.
    ImGui::SetNextWindowSizeConstraints({ 0.0f, 0.0f }, available);

    ImVec2 position = minimum;
    if (auto* window = ImGui::FindWindowByName(title))
    {
        // ImGui's default position clamp only keeps the title bar reachable.
        // Clamp the whole menu, using the next auto-fit size rather than a stale size.
        const auto size = ImGui::CalcWindowNextAutoFitSize(window);
        position.x = ImClamp(window->Pos.x, minimum.x, ImMax(minimum.x, maximum.x - size.x));
        position.y = ImClamp(window->Pos.y, minimum.y, ImMax(minimum.y, maximum.y - size.y));
    }
    ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    // Content can grow this frame before auto-fit measurements catch up. Bound
    // it to the space below/right of the chosen position; overflow stays scrollable.
    ImGui::SetNextWindowSizeConstraints({ 0.0f, 0.0f },
                                        { ImMax(1.0f, maximum.x - position.x), ImMax(1.0f, maximum.y - position.y) });
}
} // namespace MenuViewport
