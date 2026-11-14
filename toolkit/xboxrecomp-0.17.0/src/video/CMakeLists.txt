add_library(xbox_video STATIC
    video_player.c
    video_pump.c
    fb_present.c
)
target_include_directories(xbox_video PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(xbox_video PUBLIC xbox_input xbox_nv2a)
if(TARGET xbox_d3d8)
    target_link_libraries(xbox_video PUBLIC xbox_d3d8)
endif()
if(WIN32)

    target_sources(xbox_video PRIVATE gpu_present.c)
    target_link_libraries(xbox_video PUBLIC mfplat mfreadwrite mfuuid mf ole32
                          d3d11 dxgi dxguid d3dcompiler winmm)
else()

    find_package(SDL2 REQUIRED)
    target_sources(xbox_video PRIVATE fb_present_sdl.c)
    target_link_libraries(xbox_video PUBLIC SDL2::SDL2)
endif()

