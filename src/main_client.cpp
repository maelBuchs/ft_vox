#include <iostream>

#include <tracy/Tracy.hpp>

#include "client/Core/App.hpp"
#include "common/Util/TracyMemory.hpp"

int main() {
    try {
        App app;
        app.run();
    } catch (const std::exception& e) {
        std::cerr << "Application failed to start: " << e.what() << "\n";
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "An unknown error occurred." << "\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
