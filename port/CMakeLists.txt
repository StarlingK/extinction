cmake_minimum_required(VERSION 3.20)

project(extinction C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../toolkit/xboxrecomp-0.17.0" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

set(GAME_SOURCES
    src/main.c
    src/avp_mouse.c
    src/avp_setup.c
    src/avp_pcmenu.c
    src/avp_keys.c
    src/avp_coop.c
    src/avp_coop_proto.c
    src/avp_layout.c
    src/avp_skirmish.c
    src/avp_editor.c
    src/avp_wide.c
    src/avp_launcher.c
    src/recomp_manual.c

)

if(WIN32)
    set(AVP_ICON_XBE "" CACHE FILEPATH "default.xbe to take the exe icon from")
    set(AVP_ICON_RC "")
    if(AVP_ICON_XBE AND EXISTS "${AVP_ICON_XBE}")
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        enable_language(RC)
        set(AVP_ICON_ICO "${CMAKE_CURRENT_BINARY_DIR}/avp_icon.ico")
        set(AVP_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/avp_icon.rc")
        add_custom_command(
            OUTPUT "${AVP_ICON_ICO}"
            COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/xbe_icon.py" "${AVP_ICON_XBE}" "${AVP_ICON_ICO}"
            DEPENDS "${AVP_ICON_XBE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/xbe_icon.py"
            VERBATIM
        )
        file(WRITE "${AVP_ICON_RC}" "1 ICON \"avp_icon.ico\"\n")
        set_source_files_properties("${AVP_ICON_RC}" PROPERTIES OBJECT_DEPENDS "${AVP_ICON_ICO}")
    endif()
    add_executable(${PROJECT_NAME} WIN32
        ${GAME_SOURCES}
        ${RECOMP_GEN_SOURCES}
        ${AVP_ICON_RC}
    )
else()
    list(REMOVE_ITEM GAME_SOURCES src/avp_launcher.c)
    add_executable(${PROJECT_NAME}
        ${GAME_SOURCES}
        ${RECOMP_GEN_SOURCES}
    )
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME AvPExtinction)
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
    target_compile_options(${PROJECT_NAME} PRIVATE -fno-strict-aliasing -fwrapv)
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        d3d11
        dxgi
        dxguid
        $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>
        winmm
        comdlg32
        bcrypt
        gdi32
        ws2_32

        dbghelp
    )
else()
    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE Threads::Threads m dl)
endif()

if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
    )

    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
endif()


