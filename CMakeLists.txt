cmake_minimum_required(VERSION 3.20)

# Byggt för sig självt utan byggtyp: Release. Uppackning och geometriavkodning av
# en stor fil är många gånger långsammare utan optimering, och inget i
# biblioteket eller testerna beror på NDEBUG. Det står före project(), som
# annars hinner välja en egen byggtyp för vissa verktygskedjor.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR
   AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES
   AND NOT CMAKE_GENERATOR MATCHES "Multi-Config|Visual Studio|Xcode")
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)")
endif()

project(HNwd VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_SHARED_LIBS "Build as shared library" OFF)

# Windows exporterar ingenting ur en DLL utan __declspec(dllexport), och utan
# exporter skapas inget importbibliotek att länka mot. CMake listar i stället
# symbolerna ur objektfilerna, så ett shared-bygge beter sig som på Linux och
# det publika API:t slipper ett exportmakro.
set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

# PÅ när HNwd byggs för sig själv, AV när ett annat projekt drar in det med
# FetchContent eller add_subdirectory — där ska bara biblioteket byggas.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(hnwd_top_level ON)
else()
    set(hnwd_top_level OFF)
endif()
option(HNWD_BUILD_TESTS "Build tests and tools" ${hnwd_top_level})

# Visaren nwdview hämtar GlazeUI vid konfigureringen. Den är PÅ när HNwd byggs
# för sig själv och AV när ett annat projekt drar in det, så ett projekt som
# använder HNwd som bibliotek hämtar ingenting. Den är oberoende av
# HNWD_BUILD_TESTS: av bygger tester och verktyg utan visaren och utan nät.
option(HNWD_BUILD_VIEWER "Build the viewer nwdview (fetches GlazeUI)" ${hnwd_top_level})

# Installationsregler och find_package-konfiguration. Av som förval när ett annat
# projekt drar in HNwd, så att dess `cmake --install` inte får med våra filer.
option(HNWD_INSTALL "Generate install rules and the HNwd CMake package" ${hnwd_top_level})

# Varningsnivån för HNwd:s egna mål. Bygget ska vara rent på den.
function(hnwd_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<CXX_COMPILER_ID:GNU>:-Wall -Wextra -Wpedantic>
        $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wall -Wextra -Wpedantic>
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /utf-8>)
    # MSVC varnar för standardfunktioner som fopen och getenv. De används
    # medvetet, och bara i tester och verktyg.
    target_compile_definitions(${target} PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
endfunction()

add_subdirectory(src)

# Paraply för den som vill ha allt: det ett program länkar. Det ger bara den
# publika headern. De interna headers i src/core/ har allmänna namn
# (geometry.h, container.h) och ska inte hamna i ett annat projekts sökväg;
# tester och verktyg, som använder dem, länkar HNwd::core uttryckligen.
#
# HNwd::HNwd är samma mål under det namn det installerade paketet ger, så att
# FetchContent och find_package(HNwd) länkas likadant.
add_library(HNwd INTERFACE)
add_library(HNwd::HNwd ALIAS HNwd)
target_link_libraries(HNwd INTERFACE HNwd_scene)
target_include_directories(HNwd INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# Modulerna som paraplyet länkar. Listan delas av DLL-kopieringen och
# installationen nedan.
set(hnwd_modules HNwd_core HNwd_scene)

# Windows har ingen RPATH. En .exe letar sina DLL:er i sin egen katalog, och
# modulerna byggs en per katalog under src/ — så ett shared-bygge länkar klart
# och vägrar sedan starta. På Linux och macOS löser RPATH:en samma sak, och
# därför gör funktionen ingenting där.
function(hnwd_stage_runtime_dlls dir)
    if(NOT WIN32 OR NOT BUILD_SHARED_LIBS)
        return()
    endif()
    # Målen räknas upp ur katalogen i stället för att listas här, så att en ny
    # körbar inte kan glömmas.
    get_property(targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
    foreach(t IN LISTS targets)
        get_target_property(type ${t} TYPE)
        if(type STREQUAL "EXECUTABLE")
            foreach(lib IN LISTS hnwd_modules)
                add_custom_command(TARGET ${t} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            $<TARGET_FILE:${lib}> $<TARGET_FILE_DIR:${t}>)
            endforeach()
        endif()
    endforeach()
endfunction()

# Anropas SIST i tests/ respektive tools/ egna CMakeLists: add_custom_command(TARGET)
# kräver att målet skapats i samma katalog, så den kan inte köras härifrån.
if(HNWD_BUILD_TESTS)
    add_subdirectory(tests)
    add_subdirectory(tools)
endif()
if(HNWD_BUILD_VIEWER)
    add_subdirectory(viewer)
endif()

# ---- Installation -----------------------------------------------------------
#
# Det installerade paketet bär biblioteken och den enda publika headern. De
# interna headers under src/ installeras inte: allt API:t når går genom HNwd.h.
# Exportnamnen är modulernas, så målen heter HNwd::core och så vidare även efter
# find_package — samma namn som aliasen i källbygget.
if(HNWD_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    foreach(t IN LISTS hnwd_modules)
        string(REPLACE "HNwd_" "" export_name ${t})
        set_target_properties(${t} PROPERTIES EXPORT_NAME ${export_name})
    endforeach()

    install(TARGETS ${hnwd_modules} HNwd
        EXPORT HNwdTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES include/HNwd.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT HNwdTargets
        NAMESPACE HNwd::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HNwd)
    configure_package_config_file(cmake/HNwdConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/HNwdConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HNwd)
    # Inom samma major är API:t kompatibelt; en ny major får inte väljas tyst.
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/HNwdConfigVersion.cmake
        COMPATIBILITY SameMajorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/HNwdConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/HNwdConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/HNwd)
endif()
