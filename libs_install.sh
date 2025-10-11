#!/bin/bash

set -e

mkdir -p external
cd external

# GLFW
if [ ! -d "SDL" ]; then
    echo "Cloning SDL..."
    git clone --depth=1 https://github.com/libsdl-org/SDL.git
fi

# GLM
if [ ! -d "glm" ]; then
    echo "Cloning GLM..."
    git clone --depth=1 https://github.com/g-truc/glm.git
fi

# stb
if [ ! -d "stb" ]; then
    echo "Downloading stb..."
    git clone --depth=1 https://github.com/nothings/stb.git
fi

echo "✅ All libraries are ready!"