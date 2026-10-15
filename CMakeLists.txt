cmake_minimum_required(VERSION 3.24)

project(VulkanTutorial LANGUAGES C CXX)

# forces c++20 globally for all targets, including dependencies
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(
    FETCHCONTENT_BASE_DIR
    "${CMAKE_SOURCE_DIR}/external"
    CACHE PATH ""
    FORCE
)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

# vulkan-hpp and vulkan_raii.hpp are included with the installed vulkan sdk
find_package(Vulkan REQUIRED)

# add_subdirectory("${CMAKE_SOURCE_DIR}/external/glfw")
file(GLOB_RECURSE SOURCES "src/*.cpp")

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
)

FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.1
)

FetchContent_Declare(
    tinyobjloader
    GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
    GIT_TAG release
)

FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
)

FetchContent_Declare(
    vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.4.0
)

FetchContent_Declare(
    tinygltf
    GIT_REPOSITORY https://github.com/syoyo/tinygltf.git
    GIT_TAG v3.0.1
)

set(TINYGLTF3_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glfw glm tinyobjloader tinygltf stb vma)

add_library(tinygltf STATIC
    "${tinygltf_SOURCE_DIR}/tiny_gltf_v3.c"
)

target_include_directories(tinygltf PUBLIC
    "${tinygltf_SOURCE_DIR}"
)

target_compile_definitions(tinygltf PUBLIC
    TINYGLTF3_ENABLE_FS
)

add_executable(${PROJECT_NAME} ${SOURCES})
# allows designated initializers
target_compile_definitions(${PROJECT_NAME} PRIVATE
    VULKAN_HPP_NO_STRUCT_CONSTRUCTORS=1
)
target_include_directories(${PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/external/stb-src") # stb is comprised of single-header libraries and does not provide a normal CMake target to link against
target_link_libraries(${PROJECT_NAME} Vulkan::Vulkan glfw glm::glm tinyobjloader tinygltf GPUOpen::VulkanMemoryAllocator ws2_32)

set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

# enforce 64-bit CPU architecte to avoid accidentally mixing incompatible build architectures
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(STATUS "Configuring for 64-bit architecture.")
else()
    message(FATAL_ERROR "Expected 64-bit architecture, but detected 32-bit. Please configure for 64-bit.")
endif()
# cmake -S . -B build -G "Visual Studio 17 2022" -A x64

# ---- XVS Phase A: compile the region shaders into SPIR-V -------------------
# glslc is included in the installed Vulkan SDK.
if(Vulkan_GLSLC_EXECUTABLE)
    set(GLSLC "${Vulkan_GLSLC_EXECUTABLE}")
else()
    find_program(GLSLC NAMES glslc
        HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/Bin32")
endif()

if(NOT GLSLC)
    message(FATAL_ERROR "glslc was not found. Install the Vulkan SDK or add its Bin directory to PATH.")
endif()

set(XVS_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(XVS_SPV_FILES)

foreach(SHADER IN ITEMS region.vert region.frag)
    set(SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER}")
    set(SHADER_BINARY "${XVS_SHADER_OUTPUT_DIR}/${SHADER}.spv")

    add_custom_command(
        OUTPUT "${SHADER_BINARY}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${XVS_SHADER_OUTPUT_DIR}"
        COMMAND "${GLSLC}" "${SHADER_SOURCE}" -o "${SHADER_BINARY}"
        DEPENDS "${SHADER_SOURCE}"
        COMMENT "Compiling ${SHADER} to SPIR-V"
        VERBATIM
    )

    list(APPEND XVS_SPV_FILES "${SHADER_BINARY}")
endforeach()

add_custom_target(XVS_Shaders DEPENDS ${XVS_SPV_FILES})
add_dependencies(${PROJECT_NAME} XVS_Shaders)

# Used by engine.cpp when loading region.vert.spv and region.frag.spv.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    XVS_SHADER_DIR="${XVS_SHADER_OUTPUT_DIR}"
)
