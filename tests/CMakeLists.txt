add_executable(HNwd_tests
    main.cpp
    alloc_probe.cpp
    test_error.cpp
    test_inflate.cpp
    test_container.cpp
    test_samples.cpp
    test_object_reader.cpp
    test_guid_store.cpp
    test_source_header.cpp
    test_geometry_record.cpp
    test_geometry_segment.cpp
    test_samples_geometry.cpp
    test_fragments.cpp
    test_hierarchy.cpp
    test_tab_reader.cpp
    test_samples_packages.cpp
    test_partition.cpp
    test_document.cpp
    test_tree.cpp
    test_limits.cpp
    test_variants.cpp
    test_spatial.cpp
    test_material.cpp
    test_pose.cpp
    test_fragments_stream.cpp
    test_readers.cpp
    test_spatial_index.cpp
    test_property_reader.cpp
    test_elements.cpp
    test_views.cpp
    test_poses.cpp
    test_synth.cpp
    test_jpeg.cpp
    test_sysmem.cpp
    test_viewer_cells.cpp
    test_lod_tree.cpp
    test_tile_scheduler.cpp
    test_governor.cpp
    test_pick.cpp
    test_instancing.cpp
    # Visarens delar utan fönster är ren std och HNwd och prövas här, också
    # utan visaren.
    ${PROJECT_SOURCE_DIR}/viewer/jpeg.cpp
    ${PROJECT_SOURCE_DIR}/viewer/model.cpp
    ${PROJECT_SOURCE_DIR}/viewer/cells.cpp
    ${PROJECT_SOURCE_DIR}/viewer/lod_tree.cpp
    ${PROJECT_SOURCE_DIR}/viewer/governor.cpp
    ${PROJECT_SOURCE_DIR}/viewer/pick.cpp
    ${PROJECT_SOURCE_DIR}/viewer/instancing.cpp
    ${PROJECT_SOURCE_DIR}/viewer/scheduler.cpp
    ${PROJECT_SOURCE_DIR}/viewer/sysmem.cpp
)
# HNwd::core uttryckligen: testerna prövar de interna tolkarna direkt.
target_link_libraries(HNwd_tests PRIVATE HNwd HNwd::core)
target_include_directories(HNwd_tests PRIVATE ${PROJECT_SOURCE_DIR}/viewer)
# Testerna startar trådar (test_readers); biblioteket gör det inte.
find_package(Threads REQUIRED)
target_link_libraries(HNwd_tests PRIVATE Threads::Threads)
hnwd_warnings(HNwd_tests)

# DLL:erna bredvid de körbara — se hnwd_stage_runtime_dlls i rot-CMakeLists.
hnwd_stage_runtime_dlls(${CMAKE_CURRENT_SOURCE_DIR})
