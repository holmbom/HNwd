# scene — det publika API:t: Document ur HNwd.h.
#
# Sätter ihop paketen som core läser vart för sig: prövar att de stämmer med
# varandra och expanderar trädet. Den enda modulen som ser den publika headern.
add_library(HNwd_scene
    document.cpp
    tree.cpp
    geometry_reader.cpp
    property_reader.cpp
    spatial_index.cpp
    elements_api.cpp
)
add_library(HNwd::scene ALIAS HNwd_scene)

# PRIVATE: core länkas in, men dess interna headers följer inte med till den
# som länkar scene.
target_link_libraries(HNwd_scene PRIVATE HNwd_core)

target_include_directories(HNwd_scene
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

hnwd_warnings(HNwd_scene)
