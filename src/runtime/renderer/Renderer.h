// Renderer.h — high-level renderer: owns the SDL window and the Vulkan backend.

#pragma once

#include <SDL3/SDL_video.h>

#include <cstdint>
#include <optional>
#include <string>

#include "Rhi.h"

namespace pictor::render
{

class Renderer
{
public:
    Renderer(const std::string& Title, int Width, int Height);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Returns false once the user has asked to close the window.
    bool pollEvents();
    void drawFrame() { Rhi->drawFrame(); }
    [[nodiscard]] std::uint64_t getFrameCount() const { return Rhi->getFrameCount(); }

private:
    SDL_Window* Window = nullptr;
    std::optional<Rhi> Rhi;
};

}  // namespace pictor::render
