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
  debug        - Compile in Debug mode
  release      - Compile in Release mode (default)
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
    local build_type=$1
    echo -e "${CYAN}⚙️  Configuring CMake ($build_type)...${NC}"
    cmake -S . -B build -DCMAKE_BUILD_TYPE=$build_type
    echo -e "${GREEN}✓ Configuration successful${NC}"
}

function should_reconfigure() {
    local build_type=$1
    if [ ! -f "build/CMakeCache.txt" ]; then
        return 0
    fi

    local current_type=$(grep "CMAKE_BUILD_TYPE:" build/CMakeCache.txt | cut -d'=' -f2)
    if [ "$current_type" != "$build_type" ]; then
        return 0
    fi

    return 1
}

function build_project() {
    local build_type=$1

    echo -e "${CYAN}🔨 Building in $build_type mode...${NC}"

    if should_reconfigure $build_type; then
        configure_cmake $build_type
    else
        echo -e "${YELLOW}ℹ Using existing configuration${NC}"
    fi

    cmake --build build --parallel
    echo -e "${GREEN}✓ Build successful${NC}"
}

function run_project() {
    local exe_path="build/ft_vox"

    if [ ! -f "$exe_path" ]; then
        echo -e "${YELLOW}⚠️  Executable doesn't exist, building required...${NC}"
        build_project "Release"
    fi

    echo -e "${CYAN}🚀 Launching ft_vox...${NC}"
    (cd build && ./ft_vox)
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
        build_project "Release"
        run_project
        ;;
    run-debug)
        build_project "Debug"
        (cd build && ./ft_vox)
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
