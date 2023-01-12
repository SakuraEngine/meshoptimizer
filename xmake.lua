set_project("MeshOptimizer")
add_rules("mode.debug", "mode.release", "mode.releasedbg", "mode.asan")

target("MeshOptimizer")
    set_kind("$(kind)")
    add_files("src/**.cpp")
    if is_kind("shared") and is_os("windows") then
        add_defines("MESHOPTIMIZER_API=__declspec(dllexport)")
        add_defines("MESHOPTIMIZER_API=__declspec(dllimport)", {public = true})
    end