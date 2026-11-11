// Renderer.cpp — implementation of the renderer: SDL window ownership and the
// event loop, delegating all rendering to the Rhi.

#include "Renderer.h"

#include <SDL3/SDL.h>

#include <stdexcept>
#include <string>

namespace pictor::render
{

Renderer::Renderer(const std::string& Title, int Width, int Height)
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

    Window = SDL_CreateWindow(Title.c_str(), Width, Height,
                              SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!Window) {
        SDL_Quit();
        throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }

    Rhi.emplace(Window);
}

Renderer::~Renderer()
{
    // The Rhi owns the Vulkan surface, so it must go before the window.
    Rhi.reset();
    if (Window) {
        SDL_DestroyWindow(Window);
    }
    SDL_Quit();
}

bool Renderer::pollEvents()
{
    bool IsQuit = false;
    SDL_Event Event;
    while (SDL_PollEvent(&Event)) {
        if (Event.type == SDL_EVENT_QUIT) {
            IsQuit = true;
        } else if (Event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
            Rhi->setFramebufferResized(true);
        }
    }
    return !IsQuit;
}

}  // namespace pictor::render
