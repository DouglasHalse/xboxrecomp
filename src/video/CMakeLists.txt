# Shared FMV player: Media Foundation decode onto a D3D8 texture.
add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
    fb_gamma.c
    gpu_present.c
)
if(EMSCRIPTEN)
    target_sources(xbox_video PRIVATE fb_present_web.c)
elseif(NOT WIN32)
    target_sources(xbox_video PRIVATE fb_present_sdl.c)
endif()
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(xbox_video PUBLIC xbox_d3d8)
if(ANDROID)
    # Android has no D3D8/GL layer (src/d3d), so the SDL dependency of the
    # framebuffer presenter is declared here. fb_present_sdl.c spells the
    # header <SDL2/SDL.h>: with a prefixed SDL2 only its own SDL2/ dir is on
    # the include path, so add the parent as well.
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_video PUBLIC SDL2::SDL2)
    target_include_directories(xbox_video PRIVATE "${SDL2_INCLUDE_DIRS}/..")
endif()
if(WIN32)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32 d3d11 dxgi)
endif()
