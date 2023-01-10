set_project("MeshOptimizer")

add_rules("mode.debug", "mode.release", "mode.releasedbg", "mode.asan")

option("build_shared")
    set_default(false)
    set_showmenu(true)
    set_description("Toggle to build MeshOptimizer as shared library.")

target("MeshOptimizer")
    if has_config("build_shared") then 
        set_kind("shared")
        if (is_os("windows")) then 
            add_defines("MESHOPTIMIZER_API=__declspec(dllimport)")
            add_defines("MESHOPTIMIZER_API=__declspec(dllexport)")
        else
            add_defines("MESHOPTIMIZER_API=__attribute__((visibility(\"default\")))")
        end
    else
        set_kind("static")
    end
    add_files("src/**.cpp")