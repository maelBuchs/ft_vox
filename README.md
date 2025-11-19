<!--
@mainpage ft_vox Documentation
-->

# ft_vox

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](https://opensource.org/licenses/Apache-2.0)
![Language](https://img.shields.io/badge/Language-C++-blue.svg)

**ft_vox** is a procedurally generated voxel world rendering engine, using the [Vulkan](https://www.vulkan.org/) graphics API and C++.

This project is part of the [42 school](https://42.fr) graphics curriculum and serves as the technical foundation to build `ft_minecraft`.

## Features

- **Procedural Generation:** The world is generated on-the-fly using noise algorithms (e.g., Perlin Noise).
- **High-Performance Rendering with Vulkan:** Leverages the modern Vulkan API for optimal performance.
- **Chunk System:** The world is divided into chunks to optimize both rendering and generation.
- **Free Camera:** A first-person camera to explore the generated world.

## Getting Started

### Prerequisites

Make sure you have the following dependencies installed:

- [Vulkan SDK](https://vulkan.lunarg.com/sdk/home)
- [CMake](https://cmake.org/) (version 3.10 or higher)
- A compatible C++ compiler (GCC, Clang, MSVC)

### Building and Running

1.  **Clone the repository:**

        git clone https://github.com/maelBuchs/ft_vox.git
        cd ft_vox

2.  **Run the build script for your operating system:**

    - **Linux**

          # Compile only
          ./build.sh

          # Compile and run in debug mode
          ./build.sh run-debug

    - **Windows (in Powershell)**

          # Compile only
          .\build.ps1

          # Compile and run
          .\build.ps1 run

## Project Status

🚧 **Under Active Development** 🚧
This project is a work in progress. New features are added regularly.

## License

This project is distributed under the Apache 2.0 License. See the `LICENSE` file for more information.
