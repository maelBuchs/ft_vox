cmake_minimum_required(VERSION 3.16)
project(ft_vox LANGUAGES CXX)

# Utiliser C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ajouter le dossier include/
include_directories(${PROJECT_SOURCE_DIR}/include)

# Récupérer tous les fichiers sources
file(GLOB_RECURSE SOURCES
    ${PROJECT_SOURCE_DIR}/src/*.cpp
)

# Créer l'exécutable
add_executable(ft_vox ${SOURCES})

# Lier les librairies (si tu les ajoutes)
# Exemple avec GLFW + Vulkan :
find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)

target_link_libraries(ft_vox PRIVATE Vulkan::Vulkan glfw)


