cmake_minimum_required(VERSION 3.20)
project(acgc_3ds_runtime C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

get_filename_component(ACGC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

file(GLOB_RECURSE GAME_C CONFIGURE_DEPENDS "${ACGC_ROOT}/src/*.c")
file(GLOB_RECURSE GAME_CXX CONFIGURE_DEPENDS "${ACGC_ROOT}/src/*.cpp")

set(EXCLUDE_C
    "/AUS/"
    "/dolphin/gx/" "/dolphin/os/" "/dolphin/vi/" "/dolphin/pad/"
    "/dolphin/card/" "/dolphin/dvd/" "/dolphin/ai/" "/dolphin/ar/"
    "/dolphin/dsp/" "/dolphin/exi/" "/dolphin/si/" "/dolphin/db/"
    "/dolphin/mtx/" "/dolphin/base/" "/dolphin/gba/"
    "/dolphin/amcstubs/" "/dolphin/OdemuExi2/" "/dolphin/odenotstub/"
    "/TRK_MINNOW_DOLPHIN/" "/Runtime.PPCEABI.H/" "/MSL_C.PPCEABI.bare.H/"
    "/Famicom/" "/MSL_C/" "/libultra/" "/furniture/"
    "/src/data/model/obj_e_boat/"
    "/jaudio_NES/internal/dsp_cardunlock.c$"
    "/jaudio_NES/internal/dsp_GBAKey.c$"
    "/libc64/malloc.c$" "/libc64/sprintf.c$" "/libc64/aprintf.c$"
    "/ReconfigBATs.c$" "/executor.c$" "/osreport.c$" "/m_card.c$"
    "/ef_effect_lib.c$" "/ac_npc_shop_common.c$"
    "/ac_animal_logo_misc.c$" "/m_item_debug.c$"
    "/actor/npc/.*_anime.c$" "/emu64_utility.c$"
)

foreach(pattern IN LISTS EXCLUDE_C)
    list(FILTER GAME_C EXCLUDE REGEX "${pattern}")
endforeach()

set(EXCLUDE_CXX
    "/AUS/" "/dolphin/" "/TRK_MINNOW_DOLPHIN/"
    "/Runtime.PPCEABI.H/" "/MSL_C.PPCEABI.bare.H/" "/Famicom/"
    "/emu64_print.cpp$" "/jsyswrapper_ext.cpp$" "/jsyswrapper_main.cpp$"
)
foreach(pattern IN LISTS EXCLUDE_CXX)
    list(FILTER GAME_CXX EXCLUDE REGEX "${pattern}")
endforeach()

set(C_AS_CXX
    "${ACGC_ROOT}/src/static/libforest/emu64/emu64.c"
    "${ACGC_ROOT}/src/static/jaudio_NES/internal/ja_calc.c"
    "${ACGC_ROOT}/src/static/jaudio_NES/internal/jammain_2.c"
    "${ACGC_ROOT}/src/static/jaudio_NES/game/game64.c"
)
set_source_files_properties(${C_AS_CXX} PROPERTIES LANGUAGE CXX)
set_source_files_properties("${ACGC_ROOT}/src/main.c"
    PROPERTIES COMPILE_DEFINITIONS "main=ac_entry")
set_source_files_properties("${ACGC_ROOT}/src/static/boot.c"
    PROPERTIES COMPILE_DEFINITIONS "main=boot_main")

add_library(acgc_runtime_objects OBJECT
    ${GAME_C}
    ${GAME_CXX}
    "${ACGC_ROOT}/pc/src/pc_assets.c"
    "${ACGC_ROOT}/pc/src/pc_aram.c"
    "${ACGC_ROOT}/pc/src/pc_card.c"
    "${ACGC_ROOT}/pc/src/pc_dvd.c"
    "${ACGC_ROOT}/pc/src/pc_gbi_runtime.c"
    "${ACGC_ROOT}/pc/src/pc_m_card.c"
    "${ACGC_ROOT}/pc/src/pc_misc.c"
    "${ACGC_ROOT}/pc/src/pc_mtx.c"
    "${ACGC_ROOT}/pc/src/pc_os.c"
    "${ACGC_ROOT}/pc/src/pc_pad.c"
    "${ACGC_ROOT}/pc/src/pc_audio.c"
    "${ACGC_ROOT}/pc/src/pc_menu_util.c"
    "${ACGC_ROOT}/pc/src/pc_text_draw.c"
    "${ACGC_ROOT}/pc/src/pc_vi.c"
    "${ACGC_ROOT}/pc/src/pc_save_bswap.c"
    "${ACGC_ROOT}/pc/src/pc_stubs.c"
    "${ACGC_ROOT}/pc/src/pc_stubs_cpp.cpp"
    "${ACGC_ROOT}/ports/3ds/source/acgc_3ds_pc_disc.c"
    "${ACGC_ROOT}/ports/3ds/source/acgc_3ds_pc_compat.c"
    "${ACGC_ROOT}/ports/3ds/source/acgc_3ds_gx.c"
    "${ACGC_ROOT}/src/static/dolphin/pad/Padclamp.c"
)

target_include_directories(acgc_runtime_objects PRIVATE
    "${ACGC_ROOT}/include"
    "${ACGC_ROOT}/src"
    "${ACGC_ROOT}"
    "${ACGC_ROOT}/pc/include"
    "${ACGC_ROOT}/ports/3ds/include"
)
target_compile_definitions(acgc_runtime_objects PRIVATE
    TARGET_PC TARGET_3DS VERSION=0 F3DEX_GBI_2 NDEBUG _LANGUAGE_C
    $<$<COMPILE_LANGUAGE:C>:nullptr=NULL>
)
target_compile_options(acgc_runtime_objects PRIVATE
    # The native runtime spends almost all of its CPU time in the decompiled
    # game and display-list interpreter.  Building that code at -O0 makes the
    # old 3DS CPU the bottleneck before the PICA200 is meaningfully loaded.
    # Keep the compatibility flags needed by the decomp while producing an
    # actual release build for ARM.
    -O2 -fomit-frame-pointer -w -fno-strict-aliasing -fwrapv
    $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

# Feed the parent Makefile the exact object set selected by this target. A
# recursive filesystem scan can pick up stale objects for sources CMake has
# since excluded, producing a subtly mixed runtime.
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/runtime-objects.rsp"
    CONTENT "\"$<JOIN:$<TARGET_OBJECTS:acgc_runtime_objects>,\"\n\">\"\n")
