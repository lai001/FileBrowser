add_executable(FileBrowserServerTest
    main.cpp
)

# test/main.cpp provides its own main(), so link plain gtest, not gtest_main
target_link_libraries(FileBrowserServerTest PRIVATE
    fb_server
    GTest::gtest
)

fb_system_links(FileBrowserServerTest)

# group by on-disk directory structure in VS (cmake/SourceGroups.cmake)
fb_source_group(FileBrowserServerTest)

add_test(NAME FileBrowserServerTest COMMAND FileBrowserServerTest)

# The test writes its sample file under the working directory, so the run is repeatable and does not
# depend on the system temp directory being writable.
set_tests_properties(FileBrowserServerTest PROPERTIES
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
