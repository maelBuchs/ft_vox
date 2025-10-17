#include <iostream>

#include "client/Core/App.hpp"
#include "common/World/BlockRegistry.hpp"
int main() {
    // try {
    //     App app;
    //     app.run();
    // } catch (const std::exception& e) {
    //     std::cerr << "Application failed to start: " << e.what() << "\n";
    //     return EXIT_FAILURE;
    // } catch (...) {
    //     std::cerr << "An unknown error occurred." << "\n";
    //     return EXIT_FAILURE;
    // }

    BlockRegistry registry;

    return EXIT_SUCCESS;
}