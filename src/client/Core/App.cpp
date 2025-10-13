#include "App.hpp"

void App::run() {
    bool running = true;
    SDL_Event event;
    while (!window.shouldClose()) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT)
                window.setShouldClose(true);
        }
        SDL_Delay(16);
    }
}
