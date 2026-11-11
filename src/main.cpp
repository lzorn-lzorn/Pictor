// Hello Vulkan — a minimal triangle rendered with Vulkan 1.0 into an SDL3
// window. The application only drives the renderer; SDL and Vulkan stay
// behind the renderer subsystem.

#include "Renderer.h"

#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    try {
        pictor::render::Renderer Renderer("Hello Vulkan (Vulkan 1.0 + SDL3)", 800, 600);

        while (Renderer.pollEvents()) {
            Renderer.drawFrame();
        }

        std::cout << "[HelloVulkan] Rendered " << Renderer.getFrameCount()
                  << " frame(s), exiting.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[HelloVulkan] Fatal error: " << e.what() << "\n";
        return 1;
    }
}
