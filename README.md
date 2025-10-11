# ft_vox
A 42 project about making a voxel environement
#include "App.hpp"
#include <cstdlib>
#include <iostream>
#include <stdexcept>

int main() {
  App app{};

  try {
    app.run();
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}