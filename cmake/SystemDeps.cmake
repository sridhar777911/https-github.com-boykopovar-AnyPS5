# Resolves the third-party libraries from the system instead of the bundled
# submodules when ANYPS5_USE_SYSTEM_DEPS is ON. Every package is required: a
# missing one fails the configure and nothing falls back to the submodule.
#
# The system packages are exposed under the same target names the bundled
# submodules use (SDL2-static, freetype, glslang, SPIRV, glslang-standalone,
# SPIRV-Tools-opt, SPIRV-Tools-static) so the rest of the build does not need
# to know which path was taken.

if(NOT ANYPS5_USE_SYSTEM_DEPS)
    return()
endif()

find_package(SDL2 CONFIG REQUIRED)
# The prx libraries are shared objects, so they need a position-independent
# SDL2. Distribution static builds are usually not PIC, so link the shared one.
if(NOT TARGET SDL2::SDL2)
    message(FATAL_ERROR "ANYPS5_USE_SYSTEM_DEPS: SDL2 was found but does not provide SDL2::SDL2")
endif()
add_library(SDL2-static INTERFACE)
target_link_libraries(SDL2-static INTERFACE SDL2::SDL2)

find_package(Freetype REQUIRED)
if(NOT TARGET Freetype::Freetype)
    message(FATAL_ERROR "ANYPS5_USE_SYSTEM_DEPS: freetype was found but does not provide Freetype::Freetype")
endif()
add_library(freetype INTERFACE)
target_link_libraries(freetype INTERFACE Freetype::Freetype)

find_package(VulkanHeaders CONFIG REQUIRED)
if(NOT TARGET Vulkan::Headers)
    message(FATAL_ERROR "ANYPS5_USE_SYSTEM_DEPS: Vulkan-Headers was found but does not provide Vulkan::Headers")
endif()
get_target_property(ANYPS5_VULKAN_INCLUDE_DIRS Vulkan::Headers INTERFACE_INCLUDE_DIRECTORIES)
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${ANYPS5_VULKAN_INCLUDE_DIRS})
check_cxx_source_compiles("#include <vulkan/vulkan.h>\nint main() { VkPhysicalDeviceMaintenance8FeaturesKHR features{}; (void)features; return 0; }" ANYPS5_VULKAN_HAS_MAINTENANCE8)
unset(CMAKE_REQUIRED_INCLUDES)
if(NOT ANYPS5_VULKAN_HAS_MAINTENANCE8)
    message(FATAL_ERROR "ANYPS5_USE_SYSTEM_DEPS: the system Vulkan-Headers do not provide VkPhysicalDeviceMaintenance8FeaturesKHR (VK_KHR_maintenance8), which the agc driver uses; install Vulkan-Headers 1.4.310 or newer")
endif()

find_package(SPIRV-Headers CONFIG REQUIRED)
if(NOT TARGET SPIRV-Headers::SPIRV-Headers)
    message(FATAL_ERROR "ANYPS5_USE_SYSTEM_DEPS: SPIRV-Headers was found but does not provide SPIRV-Headers::SPIRV-Headers")
endif()

find_package(glslang CONFIG REQUIRED)
if(NOT TARGET glslang::glslang OR NOT TARGET glslang::SPIRV)
    message(FATAL_ERROR "ANYPS5_USE_SYSTEM_DEPS: glslang was found but does not provide glslang::glslang and glslang::SPIRV")
endif()
add_library(glslang INTERFACE)
target_link_libraries(glslang INTERFACE glslang::glslang)
add_library(SPIRV INTERFACE)
target_link_libraries(SPIRV INTERFACE glslang::SPIRV)

# The bundled glslang has both <SPIRV/...> and <glslang/...> under one root; the
# system install keeps <glslang/...> under <prefix>/include and the rest,
# including <SPIRV/GlslangToSpv.h>, under <prefix>/include/glslang.
find_path(ANYPS5_GLSLANG_PUBLIC_INCLUDE_DIR NAMES glslang/Public/ShaderLang.h)
if(NOT ANYPS5_GLSLANG_PUBLIC_INCLUDE_DIR)
    message(FATAL_ERROR "ANYPS5_USE_SYSTEM_DEPS: glslang/Public/ShaderLang.h was not found; the glslang headers are incomplete")
endif()
set(ANYPS5_GLSLANG_SPIRV_INCLUDE_DIR "${ANYPS5_GLSLANG_PUBLIC_INCLUDE_DIR}/glslang")
if(NOT EXISTS "${ANYPS5_GLSLANG_SPIRV_INCLUDE_DIR}/SPIRV/GlslangToSpv.h")
    message(FATAL_ERROR "ANYPS5_USE_SYSTEM_DEPS: ${ANYPS5_GLSLANG_SPIRV_INCLUDE_DIR}/SPIRV/GlslangToSpv.h was not found")
endif()

find_program(ANYPS5_GLSLANG_STANDALONE_EXECUTABLE NAMES glslang glslangValidator)
if(NOT ANYPS5_GLSLANG_STANDALONE_EXECUTABLE)
    message(FATAL_ERROR "ANYPS5_USE_SYSTEM_DEPS: the glslang compiler (glslang or glslangValidator) was not found; install the glslang tools package")
endif()
add_executable(glslang-standalone IMPORTED GLOBAL)
set_target_properties(glslang-standalone PROPERTIES IMPORTED_LOCATION "${ANYPS5_GLSLANG_STANDALONE_EXECUTABLE}")

if(ANYPS5_ENABLE_SPIRV_TOOLS)
    find_package(SPIRV-Tools-opt CONFIG REQUIRED)
    if(NOT TARGET SPIRV-Tools-opt OR NOT TARGET SPIRV-Tools-static)
        message(FATAL_ERROR "ANYPS5_USE_SYSTEM_DEPS: SPIRV-Tools was found but does not provide SPIRV-Tools-opt and SPIRV-Tools-static")
    endif()
endif()

set(_anyps5_system_include_targets
        SDL2::SDL2
        Freetype::Freetype
        Vulkan::Headers
        SPIRV-Headers::SPIRV-Headers
        glslang::glslang
        glslang::SPIRV
)
set(_anyps5_system_includes "")
foreach(_target IN LISTS _anyps5_system_include_targets)
    if(TARGET ${_target})
        get_target_property(_target_includes ${_target} INTERFACE_INCLUDE_DIRECTORIES)
        if(_target_includes)
            list(APPEND _anyps5_system_includes ${_target_includes})
        endif()
    endif()
endforeach()
list(APPEND _anyps5_system_includes ${ANYPS5_GLSLANG_SPIRV_INCLUDE_DIR} ${ANYPS5_GLSLANG_PUBLIC_INCLUDE_DIR})
include_directories(SYSTEM ${_anyps5_system_includes})
