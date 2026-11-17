cmake_minimum_required(VERSION 3.20)

project(extinction C CXX)

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
    src/avp_prompts.c
    src/avp_coop.c
    src/avp_coop_proto.c
    src/avp_steam.c
    src/avp_layout.c
    src/avp_skirmish.c
    src/avp_pack.c
    src/avp_nmap.c
    src/avp_mapext.c
    src/avp_editor.c
    src/avp_wide.c
    src/avp_launcher.c
    src/avp_tools.c
    src/recomp_manual.c

)

set(AVP_STEAM_DIR "" CACHE PATH "Folder with the Steamworks redistributable (steam_api64.dll or libsteam_api.so) to embed; optional")
set(AVP_STEAM_RC "")
set(AVP_STEAM_SRC "")
if(AVP_STEAM_DIR)
    if(WIN32)
        set(AVP_STEAM_FILE "${AVP_STEAM_DIR}/steam_api64.dll")
    else()
        set(AVP_STEAM_FILE "${AVP_STEAM_DIR}/libsteam_api.so")
    endif()
    if(EXISTS "${AVP_STEAM_FILE}")
        file(SHA256 "${AVP_STEAM_FILE}" AVP_STEAM_SHA256)
        message(STATUS "Embedding the Steam library (sha256 ${AVP_STEAM_SHA256})")
        add_compile_definitions(AVP_STEAM_EMBED=1)
        if(WIN32)
            enable_language(RC)
            set(AVP_STEAM_RC "${CMAKE_CURRENT_BINARY_DIR}/avp_steam.rc")
            file(WRITE "${AVP_STEAM_RC}" "AVPSTEAM RCDATA \"${AVP_STEAM_FILE}\"\n")
            set_source_files_properties("${AVP_STEAM_RC}" PROPERTIES OBJECT_DEPENDS "${AVP_STEAM_FILE}")
        else()
            set(AVP_STEAM_SRC "${CMAKE_CURRENT_BINARY_DIR}/avp_steam_blob.c")
            file(WRITE "${AVP_STEAM_SRC}" "__asm__(\".section .rodata\\n.global avp_steam_blob_start\\n.global avp_steam_blob_end\\n.balign 16\\navp_steam_blob_start:\\n.incbin \\\"${AVP_STEAM_FILE}\\\"\\navp_steam_blob_end:\\n.byte 0\\n.previous\\n\");\n")
            set_source_files_properties("${AVP_STEAM_SRC}" PROPERTIES OBJECT_DEPENDS "${AVP_STEAM_FILE}")
        endif()
    else()
        message(STATUS "AVP_STEAM_DIR has no ${AVP_STEAM_FILE}; building without the Steam library")
    endif()
endif()

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
    set(AVP_EXTRA_RES ${AVP_ICON_RC} ${AVP_STEAM_RC})
    set(AVP_WIN_SUBSYSTEM WIN32)
else()
    list(REMOVE_ITEM GAME_SOURCES src/avp_launcher.c)
    set(AVP_EXTRA_RES ${AVP_STEAM_SRC})
    set(AVP_WIN_SUBSYSTEM "")
endif()

add_library(avp_gen OBJECT ${RECOMP_GEN_SOURCES})
target_include_directories(avp_gen PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${XBOXRECOMP_DIR}/include
)
if(NOT WIN32)
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
    target_compile_options(avp_gen PRIVATE -fno-strict-aliasing -fwrapv)
endif()
if(MSVC)
    target_compile_options(avp_gen PRIVATE /bigobj)
endif()

set(AVP_TARGETS "")
function(avp_add_exe tgt out_name defs)
    add_executable(${tgt} ${AVP_WIN_SUBSYSTEM} ${GAME_SOURCES} $<TARGET_OBJECTS:avp_gen> ${AVP_EXTRA_RES} ${ARGN})
    set_target_properties(${tgt} PROPERTIES OUTPUT_NAME ${out_name})
    if(defs)
        target_compile_definitions(${tgt} PRIVATE ${defs})
    endif()
    list(APPEND AVP_TARGETS ${tgt})
    set(AVP_TARGETS ${AVP_TARGETS} PARENT_SCOPE)
endfunction()

option(AVP_EMBED_TOOLS "Embed the editor into the multiplayer program, which unpacks it next to itself" ON)
set(AVP_TOOLS_EXTRA "")
set(AVP_MP_DEFS "AVP_MP_MODE=1")
if(AVP_EMBED_TOOLS)
    set(AVP_TOOLS_DIR "${CMAKE_CURRENT_BINARY_DIR}/embed")
    file(MAKE_DIRECTORY "${AVP_TOOLS_DIR}")
    if(CMAKE_STRIP)
        set(AVP_STRIP_CMD ${CMAKE_STRIP})
    else()
        find_program(AVP_STRIP_CMD NAMES strip)
    endif()
    set(AVP_ED_BIN "${AVP_TOOLS_DIR}/ed.bin")
    if(AVP_STRIP_CMD AND NOT MSVC)
        add_custom_command(
            OUTPUT "${AVP_ED_BIN}"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:nostromo_editor>" "${AVP_ED_BIN}"
            COMMAND ${AVP_STRIP_CMD} "${AVP_ED_BIN}"
            DEPENDS nostromo_editor
            VERBATIM
        )
    else()
        add_custom_command(
            OUTPUT "${AVP_ED_BIN}"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:nostromo_editor>" "${AVP_ED_BIN}"
            DEPENDS nostromo_editor
            VERBATIM
        )
    endif()
    if(WIN32)
        enable_language(RC)
        set(AVP_TOOLS_RC "${CMAKE_CURRENT_BINARY_DIR}/avp_tools.rc")
        file(WRITE "${AVP_TOOLS_RC}" "AVPEDEXE RCDATA \"${AVP_ED_BIN}\"\n")
        set_source_files_properties("${AVP_TOOLS_RC}" PROPERTIES OBJECT_DEPENDS "${AVP_ED_BIN}")
        set(AVP_TOOLS_EXTRA "${AVP_TOOLS_RC}")
    else()
        set(AVP_TOOLS_SRC "${CMAKE_CURRENT_BINARY_DIR}/avp_tools_blob.c")
        file(WRITE "${AVP_TOOLS_SRC}" "__asm__(\".section .rodata\\n.global avp_ed_blob_start\\n.global avp_ed_blob_end\\n.balign 16\\navp_ed_blob_start:\\n.incbin \\\"${AVP_ED_BIN}\\\"\\navp_ed_blob_end:\\n.byte 0\\n.previous\\n\");\n")
        set_source_files_properties("${AVP_TOOLS_SRC}" PROPERTIES OBJECT_DEPENDS "${AVP_ED_BIN}")
        set(AVP_TOOLS_EXTRA "${AVP_TOOLS_SRC}")
    endif()
    list(APPEND AVP_MP_DEFS "AVP_TOOLS_EMBED=1")
endif()

if(WIN32)
    avp_add_exe(${PROJECT_NAME} extinction "")
else()
    avp_add_exe(${PROJECT_NAME} AvPExtinction "")
endif()
avp_add_exe(extinction_mp AvPExtinctionMP "${AVP_MP_DEFS}" ${AVP_TOOLS_EXTRA})
set(AVP_EDITOR_INTRO "" CACHE FILEPATH "Sound clip (mp3) to embed into the Nostromo Editor for its first-run splash; optional, also read from the environment variable of the same name")
if(NOT AVP_EDITOR_INTRO AND DEFINED ENV{AVP_EDITOR_INTRO})
    set(AVP_EDITOR_INTRO "$ENV{AVP_EDITOR_INTRO}")
endif()
set(EDITOR_INTRO_EXTRA "")
set(EDITOR_INTRO_DEFS "")
if(AVP_EDITOR_INTRO)
    if(EXISTS "${AVP_EDITOR_INTRO}")
        file(SHA256 "${AVP_EDITOR_INTRO}" EDITOR_INTRO_SHA256)
        message(STATUS "Embedding the editor intro clip (sha256 ${EDITOR_INTRO_SHA256})")
        set(EDITOR_INTRO_DEFS AVP_INTRO_EMBED=1)
        if(WIN32)
            set(EDITOR_INTRO_RC "${CMAKE_CURRENT_BINARY_DIR}/editor_intro.rc")
            file(WRITE "${EDITOR_INTRO_RC}" "AVPINTRO RCDATA \"${AVP_EDITOR_INTRO}\"\n")
            set_source_files_properties("${EDITOR_INTRO_RC}" PROPERTIES OBJECT_DEPENDS "${AVP_EDITOR_INTRO}")
            set(EDITOR_INTRO_EXTRA "${EDITOR_INTRO_RC}")
        else()
            set(EDITOR_INTRO_SRC "${CMAKE_CURRENT_BINARY_DIR}/editor_intro_blob.c")
            file(WRITE "${EDITOR_INTRO_SRC}" "__asm__(\".section .rodata\\n.global avp_intro_blob_start\\n.global avp_intro_blob_end\\n.balign 16\\navp_intro_blob_start:\\n.incbin \\\"${AVP_EDITOR_INTRO}\\\"\\navp_intro_blob_end:\\n.byte 0\\n.previous\\n\");\n")
            set_source_files_properties("${EDITOR_INTRO_SRC}" PROPERTIES OBJECT_DEPENDS "${AVP_EDITOR_INTRO}")
            set(EDITOR_INTRO_EXTRA "${EDITOR_INTRO_SRC}")
        endif()
    else()
        message(STATUS "AVP_EDITOR_INTRO does not name a file; building the editor without the intro sound")
    endif()
endif()
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui")
set(EDITOR_SOURCES
    editor/main.cpp
    editor/ui_gl.cpp
    editor/glmin.c
    editor/gamedata.cpp
    editor/model.cpp
    editor/texture.cpp
    editor/ui_skin.cpp
    editor/audio.cpp
    src/avp_nmap.c
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
)
if(WIN32)
    add_executable(nostromo_editor WIN32 ${EDITOR_SOURCES} editor/plat_win32.c ${AVP_ICON_RC} ${EDITOR_INTRO_EXTRA})
    target_link_libraries(nostromo_editor PRIVATE opengl32 gdi32 user32 shell32 winmm)
    target_link_options(nostromo_editor PRIVATE -static -static-libgcc -static-libstdc++)
else()
    add_executable(nostromo_editor ${EDITOR_SOURCES} editor/plat_x11.c ${EDITOR_INTRO_EXTRA})
    target_link_libraries(nostromo_editor PRIVATE X11 GL dl m)
    target_link_options(nostromo_editor PRIVATE -static-libgcc -static-libstdc++)
endif()
set_target_properties(nostromo_editor PROPERTIES OUTPUT_NAME NostromoEditor CXX_STANDARD 14)
target_include_directories(nostromo_editor PRIVATE editor src ${IMGUI_DIR} third_party/minimp3)
target_compile_definitions(nostromo_editor PRIVATE
    ${EDITOR_INTRO_DEFS}
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS
    IMGUI_DISABLE_WIN32_DEFAULT_IME_FUNCTIONS
    IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
    IMGUI_DISABLE_FILE_FUNCTIONS)
if(AVP_EMBED_TOOLS)
    add_dependencies(extinction_mp nostromo_editor)
endif()

foreach(tgt ${AVP_TARGETS})
    target_link_libraries(${tgt} PRIVATE xboxrecomp)
    target_include_directories(${tgt} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
        ${XBOXRECOMP_DIR}/include
    )
    if(WIN32)
        target_link_libraries(${tgt} PRIVATE
            d3d11
            dxgi
            dxguid
            $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>
            winmm
            comdlg32
            bcrypt
            gdi32
            ws2_32
            shell32
            dbghelp
        )
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(${tgt} PRIVATE Threads::Threads m dl)
        target_compile_options(${tgt} PRIVATE -fno-strict-aliasing -fwrapv)
    endif()
    if(MSVC)
        target_link_options(${tgt} PRIVATE /LARGEADDRESSAWARE)
        target_compile_options(${tgt} PRIVATE /bigobj)
    endif()
endforeach()
