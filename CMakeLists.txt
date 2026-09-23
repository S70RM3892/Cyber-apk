cmake_minimum_required(VERSION 3.22)
project(cyber_apex LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    set(APEX_WARNINGS /W4)
else()
    set(APEX_WARNINGS -Wall -Wextra -Wpedantic -Wconversion)
endif()
# Vulkan code zero-initialises structs with `VkFooInfo info{VK_STRUCTURE_TYPE_...}`,
# which is idiomatic but trips -Wmissing-field-initializers.
if (MSVC)
    set(APEX_VK_WARNINGS ${APEX_WARNINGS})
else()
    set(APEX_VK_WARNINGS ${APEX_WARNINGS} -Wno-missing-field-initializers)
endif()

# ---- Core: platform-independent game + city code (host tools, tests, runtime) ----
add_library(apex_core
    engine/src/specular_aa.cpp
    engine/src/cluster_cull.cpp
    engine/src/citygen.cpp
    engine/src/world.cpp
    engine/src/buildgen.cpp
    engine/src/game.cpp
    engine/src/hud.cpp
    engine/src/audio.cpp
)
target_include_directories(apex_core PUBLIC engine/include)
target_compile_options(apex_core PRIVATE ${APEX_WARNINGS})
find_package(Threads REQUIRED)
target_link_libraries(apex_core PUBLIC Threads::Threads)

# ---- Shaders: GLSL -> SPIR-V -> embedded C++ header ----
# glslc ships with the NDK; glslangValidator comes with the Vulkan SDK / distro packages.
if (ANDROID AND EXISTS "${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}/glslc")
    set(APEX_GLSLC "${ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}/glslc")
else()
    find_program(APEX_GLSLC glslc)
endif()
find_program(APEX_GLSLANG glslangValidator)

set(APEX_SHADERS
    buildings.vert buildings.frag ground.vert ground.frag signs.vert signs.frag
    fullscreen.vert sky.frag rain.vert rain.frag traffic.vert traffic.frag hud.vert hud.frag
    streetlife.vert streetlife.frag beacon.vert beacon.frag signs_glow.frag
    props.vert props.frag lights.vert lights.frag infra.vert infra.frag detail.vert detail.frag halo.vert halo.frag
    resolve.frag bloom_down.frag bloom_up.frag tonemap.frag
)
file(GLOB APEX_SHADER_INCLUDES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/include/*.glsl)
set(APEX_SPV_DIR ${CMAKE_CURRENT_BINARY_DIR}/spv)
set(APEX_SPVS)
foreach(s IN LISTS APEX_SHADERS)
    set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${s})
    set(spv ${APEX_SPV_DIR}/${s}.spv)
    if (APEX_GLSLC)
        set(cmd ${APEX_GLSLC} --target-env=vulkan1.3 -O -o ${spv} ${src})
    elseif (APEX_GLSLANG)
        set(cmd ${APEX_GLSLANG} -V --target-env vulkan1.3 -o ${spv} ${src})
    else()
        message(FATAL_ERROR "Need glslc or glslangValidator to build shaders")
    endif()
    add_custom_command(OUTPUT ${spv}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${APEX_SPV_DIR}
        COMMAND ${cmd}
        DEPENDS ${src} ${APEX_SHADER_INCLUDES}
        COMMENT "GLSL -> SPIR-V ${s}" VERBATIM)
    list(APPEND APEX_SPVS ${spv})
endforeach()
set(APEX_SHADER_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/shaders_embedded.hpp)
add_custom_command(OUTPUT ${APEX_SHADER_HEADER}
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${APEX_SHADER_HEADER} "-DINPUTS=${APEX_SPVS}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
    DEPENDS ${APEX_SPVS} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake
    COMMENT "Embedding SPIR-V" VERBATIM)
set(APEX_FONT_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/sign_font_sdf.hpp)
add_custom_command(OUTPUT ${APEX_FONT_HEADER}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/assets/sign_font_sdf.bin
            -DOUTPUT=${APEX_FONT_HEADER} -DNAME=sign_font_sdf
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/sign_font_sdf.bin ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake
    COMMENT "Embedding sign font atlas" VERBATIM)
add_custom_target(apex_shaders DEPENDS ${APEX_SHADER_HEADER} ${APEX_FONT_HEADER})

# ---- Renderer (Vulkan 1.3) ----
find_package(Vulkan QUIET)
add_library(apex_renderer
    renderer/vk_context.cpp
    renderer/vk_resources.cpp
    renderer/renderer.cpp
    renderer/presenter.cpp
)
add_dependencies(apex_renderer apex_shaders)
target_include_directories(apex_renderer PUBLIC renderer ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(apex_renderer PRIVATE ${APEX_VK_WARNINGS})
if (ANDROID)
    target_link_libraries(apex_renderer PUBLIC apex_core vulkan log)
else()
    target_link_libraries(apex_renderer PUBLIC apex_core Vulkan::Vulkan)
endif()

if (ANDROID)
    # ---- APK native library (NativeActivity) ----
    add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
    target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)
    add_library(cyberapex SHARED android/src/main/cpp/android_main.cpp)
    target_compile_options(cyberapex PRIVATE ${APEX_VK_WARNINGS})
    target_link_libraries(cyberapex PRIVATE apex_renderer native_app_glue android log aaudio)
    # Keep ANativeActivity_onCreate exported from the static glue library.
    set_target_properties(cyberapex PROPERTIES LINK_FLAGS "-u ANativeActivity_onCreate")
else()
    # ---- Headless host runner ----
    find_package(ZLIB REQUIRED)
    add_executable(apex_host host/main.cpp)
    target_compile_options(apex_host PRIVATE ${APEX_VK_WARNINGS})
    target_link_libraries(apex_host PRIVATE apex_renderer ZLIB::ZLIB)

    include(CTest)
    if (BUILD_TESTING)
        add_executable(apex_tests tests/test_main.cpp)
        target_link_libraries(apex_tests PRIVATE apex_core)
        add_test(NAME apex_tests COMMAND apex_tests)
    endif()
endif()
