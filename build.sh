#!/bin/bash
# Usage: ./build.sh [clean|debug|release|run|help]

set -e

ACTION="${1:-debug}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

function show_help() {
    cat << EOF
ft_vox - Build script
=====================

Usage: ./build.sh [action]

Available actions:
  clean        - Clean the build directory
  debug        - Compile in Debug mode (default)
  release      - Compile in Release mode
  run          - Compile and run in Release mode
  run-debug    - Compile and run in Debug mode
  help         - Show this help

Examples:
  ./build.sh                # Compile in Debug
  ./build.sh debug          # Compile in Debug
  ./build.sh run            # Compile and run in Release
  ./build.sh clean          # Clean the build

EOF
}

function clean_build() {
    echo -e "${CYAN}🧹 Cleaning build directory...${NC}"
    if [ -d "build" ]; then
        rm -rf build/*
        echo -e "${GREEN}✓ Build cleaned${NC}"
    else
        echo -e "${YELLOW}ℹ No build directory to clean${NC}"
    fi
}

function configure_cmake() {
    local config=$1
    echo -e "${CYAN}⚙️  Configuring CMake with Ninja Multi-Config (${config} mode)...${NC}"

    # Force use of clang/clang++ as compiler
    export CC=clang
    export CXX=clang++

    cmake -S . -B build -G "Ninja Multi-Config" \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++

    if [ $? -ne 0 ]; then
        echo -e "${RED}❌ Error during CMake configuration${NC}"
        echo -e "${YELLOW}ℹ Make sure Ninja and clang are installed${NC}"
        exit 1
    fi
    echo -e "${GREEN}✓ Configuration successful${NC}"
}

function should_reconfigure() {
    local config=$1

    # With Ninja Multi-Config, we need to reconfigure if cache doesn't exist
    # or if build type has changed
    if [ ! -f "build/CMakeCache.txt" ]; then
        return 0
    fi

    # Check if build type has changed
    local current_config=$(grep "CMAKE_BUILD_TYPE:STRING=" build/CMakeCache.txt | cut -d'=' -f2)
    if [ "$current_config" != "$config" ]; then
        echo -e "${YELLOW}ℹ Configuration change: $current_config -> $config${NC}"
        return 0
    fi

    return 1
}

function build_project() {
    local config=$1

    echo -e "${CYAN}🔨 Building in $config mode...${NC}"

    if should_reconfigure $config; then
        configure_cmake $config
    else
        echo -e "${YELLOW}ℹ Using existing configuration${NC}"
    fi

    # Use all available CPU cores for faster compilation
    # With Ninja Multi-Config, we need to specify --config
    cmake --build build --config $config --parallel

    if [ $? -ne 0 ]; then
        echo -e "${RED}❌ Error during compilation${NC}"
        exit 1
    fi

    # Copy compile_commands.json to root for LSP tools
    if [ -f "build/compile_commands.json" ]; then
        cp build/compile_commands.json .
        echo -e "${GREEN}✓ compile_commands.json copied to root${NC}"
    fi

    echo -e "${GREEN}✓ Build successful${NC}"
}

function run_project() {
    local config=$1

    # Always build before running - Ninja is smart and only recompiles what changed
    # This ensures we always run the latest version
    build_project "$config"

    # With Ninja Multi-Config, executables are in build/<Config> directory
    local exe_path="build/${config}/ft_vox"

    echo -e "${CYAN}🚀 Launching ft_vox ($config)...${NC}"
    $exe_path
}

case "$ACTION" in
    clean)
        clean_build
        ;;
    debug)
        build_project "Debug"
        ;;
    release)
        build_project "Release"
        ;;
    run)
        run_project "Release"
        ;;
    run-debug)
        run_project "Debug"
        ;;
    help)
        show_help
        exit 0
        ;;
    *)
        echo -e "${RED}❌ Unknown action: $ACTION${NC}"
        show_help
        exit 1
        ;;
esac

echo ""
echo -e "${GREEN}✨ Done!${NC}"
