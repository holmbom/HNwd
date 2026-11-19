#include <cstdio>
#include "HNwd.h"
#include "test_data.h"

int test_error();
int test_inflate();
int test_dbcache();
int test_container();
int test_samples();
int test_prefixed();
int test_object_reader();
int test_guid_store();
int test_source_header();
int test_geometry_record();
int test_geometry_segment();
int test_samples_geometry();
int test_fragments();
int test_hierarchy();
int test_tab_reader();
int test_samples_packages();
int test_partition();
int test_document();
int test_tree();
int test_limits();
int test_variants();
int test_spatial();
int test_material();
int test_pose();
int test_fragments_stream();
int test_readers();
int test_spatial_index();
int test_property_reader();
int test_elements();
int test_views();
int test_poses();
int test_synth();
int test_jpeg();
int test_sysmem();
int test_viewer_cells();
int test_lod_tree();
int test_tile_scheduler();
int test_governor();
int test_pick();
int test_instancing();

int main() {
    std::printf("HNwd %s\n", HNwd::kVersionString);

    int fails = 0;
    fails += test_error();
    fails += test_inflate();
    fails += test_dbcache();
    fails += test_container();
    fails += test_samples();
    fails += test_prefixed();
    fails += test_object_reader();
    fails += test_guid_store();
    fails += test_source_header();
    fails += test_geometry_record();
    fails += test_geometry_segment();
    fails += test_samples_geometry();
    fails += test_fragments();
    fails += test_hierarchy();
    fails += test_tab_reader();
    fails += test_samples_packages();
    fails += test_partition();
    fails += test_document();
    fails += test_tree();
    fails += test_limits();
    fails += test_variants();
    fails += test_spatial();
    fails += test_material();
    fails += test_pose();
    fails += test_fragments_stream();
    fails += test_readers();
    fails += test_spatial_index();
    fails += test_property_reader();
    fails += test_elements();
    fails += test_views();
    fails += test_poses();
    fails += test_synth();
    fails += test_jpeg();
    fails += test_sysmem();
    fails += test_viewer_cells();
    fails += test_lod_tree();
    fails += test_tile_scheduler();
    fails += test_governor();
    fails += test_pick();
    fails += test_instancing();

    if (fails != 0)
        std::printf("%d test(s) FAILED.\n", fails);
    else if (g_skipped != 0)
        std::printf("All tests passed, but %d SKIPPED for missing test data "
                    "(run from the repository root, see tests/test_data.h).\n",
                    g_skipped);
    else
        std::printf("All tests passed.\n");

    return fails;
}
