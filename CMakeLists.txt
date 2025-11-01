cmake_minimum_required(VERSION 3.16)
project(ft_vox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

# Mark SDL includes as SYSTEM to suppress warnings
set_target_properties(SDL3-shared PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES $<TARGET_PROPERTY:SDL3-shared,INTERFACE_INCLUDE_DIRECTORIES>)

# 2. GLM
FetchContent_Declare(
  glm
  GIT_REPOSITORY https://github.com/g-truc/glm.git
  GIT_TAG        1.0.1
)
FetchContent_MakeAvailable(glm)

# Mark GLM includes as SYSTEM to suppress warnings
set_target_properties(glm PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES $<TARGET_PROPERTY:glm,INTERFACE_INCLUDE_DIRECTORIES>)

# 3. STB
FetchContent_Declare(
  stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        master
)
FetchContent_MakeAvailable(stb)

# 4. vk-bootstrap (Vulkan initialization helper)
FetchContent_Declare(
  vk-bootstrap
  GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
  GIT_TAG        v1.4.329
)
FetchContent_MakeAvailable(vk-bootstrap)

# Mark vk-bootstrap includes as SYSTEM to suppress warnings
set_target_properties(vk-bootstrap PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES $<TARGET_PROPERTY:vk-bootstrap,INTERFACE_INCLUDE_DIRECTORIES>)

# 5. VMA (Vulkan Memory Allocator)
FetchContent_Declare(
  VulkanMemoryAllocator
  GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
  GIT_TAG        v3.3.0
)
FetchContent_MakeAvailable(VulkanMemoryAllocator)

# Mark VMA includes as SYSTEM to suppress warnings
set_target_properties(VulkanMemoryAllocator PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES $<TARGET_PROPERTY:VulkanMemoryAllocator,INTERFACE_INCLUDE_DIRECTORIES>)

# 6. Dear ImGui
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.92.4
)
FetchContent_MakeAvailable(imgui)

# 7. nlohmann/json (JSON parsing)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
)
FetchContent_MakeAvailable(nlohmann_json)

# Mark nlohmann_json includes as SYSTEM to suppress warnings
set_target_properties(nlohmann_json PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES $<TARGET_PROPERTY:nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>)

# 8. Tracy Profiler
set(TRACY_ENABLE ON CACHE BOOL "" FORCE)
set(TRACY_ON_DEMAND ON CACHE BOOL "" FORCE)
FetchContent_Declare(
    tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy.git
    GIT_TAG        v0.12.2
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(tracy)

# Mark Tracy includes as SYSTEM to suppress warnings
set_target_properties(TracyClient PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES $<TARGET_PROPERTY:TracyClient,INTERFACE_INCLUDE_DIRECTORIES>)

# ImGui requires manual setup - create library with backends
add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(imgui SYSTEM PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(imgui PUBLIC
  SDL3::SDL3
  Vulkan::Vulkan
)


# --- Project Structure ---

include_directories(SYSTEM ${stb_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/src)

# --- Shader Compilation ---

find_program(GLSLC glslc REQUIRED HINTS
    "$ENV{VULKAN_SDK}/Bin"
    "$ENV{VULKAN_SDK}/bin"
)

file(GLOB_RECURSE SHADER_SOURCES
    "${CMAKE_SOURCE_DIR}/shaders/glsl/voxel.vert"
    "${CMAKE_SOURCE_DIR}/shaders/glsl/voxel.frag"
    "${CMAKE_SOURCE_DIR}/shaders/glsl/sky.vert"
    "${CMAKE_SOURCE_DIR}/shaders/glsl/sky.frag"
)

set(SPIRV_SHADERS "")

if(CMAKE_CONFIGURATION_TYPES OR CMAKE_GENERATOR MATCHES "Visual Studio" OR MSVC)
    set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/${CMAKE_CFG_INTDIR}/shaders")
else()
    set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
endif()

file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# For each shader source, create a custom command that produces a .spv and depends on the .glsl
foreach(SHADER_SRC ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER_SRC} NAME)
    set(SHADER_SPV "${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv")

    add_custom_command(
        OUTPUT ${SHADER_SPV}
        COMMAND ${GLSLC} --target-env=vulkan1.4 -O ${SHADER_SRC} -o ${SHADER_SPV}
        DEPENDS ${SHADER_SRC}
        COMMENT "Compiling shader: ${SHADER_NAME}"
    )

    list(APPEND SPIRV_SHADERS ${SHADER_SPV})
endforeach()

# Single checkpoint target depending on all generated SPIR-V files.
add_custom_target(CompileShaders ALL DEPENDS ${SPIRV_SHADERS})

# Collect source files
set(CLIENT_SOURCES
    ${CMAKE_SOURCE_DIR}/src/client/Core/App.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Core/Window.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Core/InputManager.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Game/Camera.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Graphics/Renderer.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Graphics/Memory/DescriptorAllocator.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Graphics/Voxel/VoxelRenderer.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Graphics/Rendering/RenderContext.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Graphics/Voxel/MeshManager.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Graphics/Rendering/FrameManager.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Graphics/Rendering/CommandExecutor.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Graphics/Voxel/MeshBufferPool.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Graphics/Pipeline/ComputePipelineBuilder.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Graphics/Pipeline/Pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Graphics/Pipeline/GraphicsPipelineBuilder.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Graphics/Core/VulkanSwapchain.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Graphics/Core/VulkanDevice.cpp
    ${CMAKE_SOURCE_DIR}/src/client/Graphics/Core/VulkanBuffer.cpp
)

set(COMMON_SOURCES
    ${CMAKE_SOURCE_DIR}/src/common/World/ChunkMesh.cpp
    ${CMAKE_SOURCE_DIR}/src/common/World/BlockRegistry.cpp
    ${CMAKE_SOURCE_DIR}/src/common/World/Chunk.cpp
    ${CMAKE_SOURCE_DIR}/src/common/Util/perlinNoise.cpp
)

# Server-side world management (used by client for async chunk loading)
set(SERVER_WORLD_SOURCES
    ${CMAKE_SOURCE_DIR}/src/server/World/WorldGeneration.cpp
    ${CMAKE_SOURCE_DIR}/src/server/World/WorldManager.cpp
    ${CMAKE_SOURCE_DIR}/src/server/World/MeshingThread.cpp
)

# Server executable
set(SERVER_SOURCES

)

add_executable(ft_vox
    src/main_client.cpp
    ${CLIENT_SOURCES}
    ${COMMON_SOURCES}
    ${SERVER_WORLD_SOURCES}
)

# Ensure shaders are compiled before building the executable
add_dependencies(ft_vox CompileShaders)


add_executable(ft_vox_server
    src/main_server.cpp
    ${SERVER_SOURCES}
    ${COMMON_SOURCES}
)

# Link the libraries to our executable "ft_vox"

# 1. Vulkan
find_package(Vulkan REQUIRED)
target_link_libraries(ft_vox PRIVATE Vulkan::Vulkan)

# 2. SDL3
target_link_libraries(ft_vox PRIVATE SDL3::SDL3)

# 3. GLM
target_link_libraries(ft_vox PRIVATE glm::glm)

# 4. vk-bootstrap
target_link_libraries(ft_vox PRIVATE vk-bootstrap::vk-bootstrap)

# 5. VMA
target_link_libraries(ft_vox PRIVATE VulkanMemoryAllocator)

# 6. ImGui
target_link_libraries(ft_vox PRIVATE imgui)

# 7. nlohmann/json
target_link_libraries(ft_vox PRIVATE nlohmann_json::nlohmann_json)

# 8. Tracy
target_link_libraries(ft_vox PRIVATE TracyClient)

# Link libraries to server
target_link_libraries(ft_vox_server PRIVATE glm::glm nlohmann_json::nlohmann_json TracyClient)

# --- Compilation Flags ---

# Set default build type if not specified (for single-configuration generators like Ninja)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# Apply compiler flags based on compiler type
if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # --- MSVC (Visual Studio) ---
    target_compile_options(ft_vox PRIVATE
        $<$<CONFIG:Debug>:/W4 /Od /Zi>
        $<$<CONFIG:Release>:/W4 /O2>
    )
    target_compile_options(ft_vox_server PRIVATE
        $<$<CONFIG:Debug>:/W4 /Od /Zi>
        $<$<CONFIG:Release>:/W4 /O2>
    )
    target_link_options(ft_vox PRIVATE $<$<CONFIG:Debug>:/DEBUG>)
    target_link_options(ft_vox_server PRIVATE $<$<CONFIG:Debug>:/DEBUG>)
    target_compile_definitions(ft_vox PRIVATE $<$<CONFIG:Release>:NDEBUG>)
    target_compile_definitions(ft_vox_server PRIVATE $<$<CONFIG:Release>:NDEBUG>)
else()
    # --- GCC/Clang (Linux and Clang on Windows) ---
    target_compile_options(ft_vox PRIVATE
        $<$<CONFIG:Debug>:-Wall -Wextra -g -O0
            -Wno-c++98-compat -Wno-c++98-compat-pedantic
            -Wno-padded -Wno-documentation
            -Wno-unused-command-line-argument>
        $<$<CONFIG:Release>:-Wall -Wextra -O3>
    )
    target_compile_options(ft_vox_server PRIVATE
        $<$<CONFIG:Debug>:-Wall -Wextra -g -O0
            -Wno-c++98-compat -Wno-c++98-compat-pedantic
            -Wno-padded -Wno-documentation
            -Wno-unused-command-line-argument>
        $<$<CONFIG:Release>:-Wall -Wextra -O3>
    )
    target_compile_definitions(ft_vox PRIVATE $<$<CONFIG:Release>:NDEBUG>)
    target_compile_definitions(ft_vox_server PRIVATE $<$<CONFIG:Release>:NDEBUG>)
endif()

# Note for STB :
# To use stb_image.h, we must define STB_IMAGE_IMPLEMENTATION
#
# #define STB_IMAGE_IMPLEMENTATION
# #include <stb_image.h>

# Copy compiled shaders to the executable directory
add_custom_command(
    TARGET ft_vox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:ft_vox>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${SHADER_OUTPUT_DIR}
        $<TARGET_FILE_DIR:ft_vox>/shaders
    COMMENT "Copying compiled shaders to executable directory"
)

if(WIN32)
    add_custom_command(
        TARGET ft_vox POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3-shared>
            $<TARGET_FILE_DIR:ft_vox>
        COMMENT "Copying SDL3.dll to the executable directory"
    )
endif()
