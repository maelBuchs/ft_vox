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

add_executable(ft_vox
    src/main.cpp
)

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
