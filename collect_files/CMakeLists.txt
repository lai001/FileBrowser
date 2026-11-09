add_executable(CollectFiles
    src/main.cpp
)

target_link_libraries(CollectFiles PRIVATE
    CLI11::CLI11
    spdlog::spdlog
    filebrowser_proto
    fb_misc
    ${CMAKE_DL_LIBS}
)

fb_system_links(CollectFiles)

# group by on-disk directory structure in VS (cmake/SourceGroups.cmake): src
fb_source_group(CollectFiles)
