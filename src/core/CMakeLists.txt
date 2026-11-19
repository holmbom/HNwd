# core — filformatet.
#
# Noll beroenden: varken på andra moduler eller på något externt. Den vet inte
# att den publika HNwd.h finns. Include-stigarna nedan är det som håller den
# gränsen — core får ingen väg till andras headers.
add_library(HNwd_core
    nwd_error.cpp
    mapped_file.cpp
    inflate.cpp
    dbcache.cpp
    container.cpp
    object_reader.cpp
    guid_store.cpp
    source_header.cpp
    material.cpp
    pose.cpp
    fragments.cpp
    partition_props.cpp
    partition.cpp
    hierarchy.cpp
    tab_reader.cpp
    geometry.cpp
    geometry_segment.cpp
    spatial.cpp
    elements.cpp
)
add_library(HNwd::core ALIAS HNwd_core)

target_include_directories(HNwd_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

hnwd_warnings(HNwd_core)
