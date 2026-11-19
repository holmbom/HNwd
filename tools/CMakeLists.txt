# Hjälpverktyg — inte del av biblioteket. Varje verktyg är en körbar som länkar
# paraplyet HNwd och tar sina argument genom tool_main.h.

# Lägger till ett verktyg ur en källfil med samma namn.
function(hnwd_tool name)
    add_executable(${name} ${name}.cpp)
    # HNwd::core uttryckligen: verktygen läser paketen genom de interna tolkarna.
    target_link_libraries(${name} PRIVATE HNwd HNwd::core)
    hnwd_warnings(${name})
    # tool_main.h ger wmain() på Windows, och MinGW länkar bara mot den med
    # den här flaggan. MSVC väljer wmain() av sig själv.
    if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(${name} PRIVATE -municode)
    endif()
endfunction()

hnwd_tool(nwddump)
hnwd_tool(nwd2obj)
hnwd_tool(nwdcheck)
hnwd_tool(nwdsynth)

# DLL:erna bredvid de körbara — se hnwd_stage_runtime_dlls i rot-CMakeLists.
hnwd_stage_runtime_dlls(${CMAKE_CURRENT_SOURCE_DIR})
