add_library(fb_misc STATIC
    Guarded.h
    Misc.cpp
    Misc.h
)
target_include_directories(fb_misc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(fb_misc PUBLIC spdlog::spdlog)

# group by on-disk directory structure in VS (cmake/SourceGroups.cmake)
fb_source_group(fb_misc)
