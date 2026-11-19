# nwdview — visaren: modellen i 3D, inläst i arbetstrådar med detalj efter
# skärmstorlek, och ett klick på ett objekt visar dess namn och egenskaper.
# Inte del av biblioteket.
#
# Visaren är det enda i HNwd som har ett beroende: fönstret och 3D-ytan kommer
# ur GlazeUI, som hämtas här vid konfigureringen. Katalogen läggs bara till när
# HNWD_BUILD_VIEWER är på, och den är av när ett annat projekt drar in HNwd, så
# ett projekt som använder HNwd som bibliotek hämtar aldrig GlazeUI.

if(CMAKE_VERSION VERSION_LESS 3.24)
    message(STATUS "HNwd: nwdview kräver CMake 3.24 eller senare och byggs inte")
    return()
endif()

include(FetchContent)

# Bara ramverket: inget nätverks- eller terminalpaket. Ett projekt som drar in
# HNwd och själv använder GlazeUI behåller sina egna val.
if(hnwd_top_level)
    set(GLAZE_NETWORKING OFF CACHE BOOL "" FORCE)
    set(GLAZE_TERMINAL OFF CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(GlazeUI
    GIT_REPOSITORY https://github.com/holmbom/GlazeUI.git
    GIT_TAG        v0.10.0
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(GlazeUI)

# model, cells, lod_tree, governor, pick, instancing, scheduler, sysmem, walk och jpeg är ren std och HNwd;
# viewer_window, textures och main är fönstret.
add_executable(nwdview WIN32 MACOSX_BUNDLE
    main.cpp
    viewer_window.cpp
    model.cpp
    cells.cpp
    lod_tree.cpp
    governor.cpp
    pick.cpp
    instancing.cpp
    scheduler.cpp
    sysmem.cpp
    jpeg.cpp
    textures.cpp
    walk.cpp)
# HNwd::core uttryckligen: visaren tar sina argument som UTF-8 genom path_utf8.h.
target_link_libraries(nwdview PRIVATE HNwd HNwd::core glaze glaze_threads_ui)
hnwd_warnings(nwdview)

hnwd_stage_runtime_dlls(${CMAKE_CURRENT_SOURCE_DIR})
