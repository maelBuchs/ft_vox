cmake_minimum_required(VERSION 3.16)
project(ft_vox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# --- Dependency Management ---

# 1. SDL3
# We can desactivate building tests and examples to speed up the process
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  SDL
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG        release-3.2.24
)

FetchContent_MakeAvailable(SDL)

# 2. GLM
FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG        1.0.1
)
FetchContent_MakeAvailable(glm)

# 3. STB
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(stb)


# --- Project Structure ---

include_directories(${stb_SOURCE_DIR})

# --- Shader Compilation ---

find_program(GLSLC glslc HINTS
    "$ENV{VULKAN_SDK}/Bin"
    "$ENV{VULKAN_SDK}/bin"
)

if(NOT GLSLC)
    message(FATAL_ERROR "glslc not found! Please install the Vulkan SDK and ensure glslc is in your PATH.")
endif()

# Collect all shader files
file(GLOB_RECURSE SHADER_SOURCES
    "${CMAKE_SOURCE_DIR}/src/shaders/*.vert"
    "${CMAKE_SOURCE_DIR}/src/shaders/*.frag"
    "${CMAKE_SOURCE_DIR}/src/shaders/*.comp"
    "${CMAKE_SOURCE_DIR}/src/shaders/*.geom"
    "${CMAKE_SOURCE_DIR}/src/shaders/*.tesc"
    "${CMAKE_SOURCE_DIR}/src/shaders/*.tese"
)

# Create output directory for compiled shaders
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# Compile each shader to SPIR-V
foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv")

    add_custom_command(
        OUTPUT ${SHADER_OUTPUT}
        COMMAND ${GLSLC} ${SHADER} -o ${SHADER_OUTPUT}
        DEPENDS ${SHADER}
        COMMENT "Compiling shader: ${SHADER_NAME}"
    )

    list(APPEND SPIRV_SHADERS ${SHADER_OUTPUT})
endforeach()

# Create a custom target for shader compilation
add_custom_target(shaders ALL DEPENDS ${SPIRV_SHADERS})

add_executable(ft_vox
    src/main.cpp
    src/Window.cpp
    src/App.cpp
    src/Pipeline.cpp
)

# Make sure shaders are compiled before building the executable
add_dependencies(ft_vox shaders)

# Link the libraries to our executable "ft_vox"

# 1. Vulkan
find_package(Vulkan REQUIRED)
target_link_libraries(ft_vox PRIVATE Vulkan::Vulkan)

# 2. SDL3
target_link_libraries(ft_vox PRIVATE SDL3::SDL3)

# 3. GLM
target_link_libraries(ft_vox PRIVATE glm::glm)

# --- Compilation Flags for ft_vox only ---
if(MSVC)
    # Debug flags
    target_compile_options(ft_vox PRIVATE $<$<CONFIG:Debug>:/W4 /Od /Zi>)
    target_link_options(ft_vox PRIVATE $<$<CONFIG:Debug>:/DEBUG>)

    # Release flags
    target_compile_options(ft_vox PRIVATE $<$<CONFIG:Release>:/W4 /O2 /GL>)
    target_link_options(ft_vox PRIVATE $<$<CONFIG:Release>:/LTCG>)
    target_compile_definitions(ft_vox PRIVATE $<$<CONFIG:Release>:NDEBUG>)
else()
    # GCC/Clang Debug flags
    target_compile_options(ft_vox PRIVATE $<$<CONFIG:Debug>:-Wall -Wextra -g -O0>)

    # GCC/Clang Release flags
    target_compile_options(ft_vox PRIVATE $<$<CONFIG:Release>:-Wall -Wextra -O3 -march=native>)
    target_compile_definitions(ft_vox PRIVATE $<$<CONFIG:Release>:NDEBUG>)
endif()

# Note for STB :
# To use stb_image.h, we must define STB_IMAGE_IMPLEMENTATION
#
# #define STB_IMAGE_IMPLEMENTATION
# #include <stb_image.h>

if(WIN32)
    add_custom_command(
        TARGET ft_vox POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3-shared>
            $<TARGET_FILE_DIR:ft_vox>
        COMMENT "Copying SDL3.dll to the executable directory"
    )
endif()
