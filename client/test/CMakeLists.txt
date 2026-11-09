add_executable(FileBrowserClientTest
    main.cpp
)

# group by on-disk directory structure in VS (cmake/SourceGroups.cmake)
fb_source_group(FileBrowserClientTest)

# test/main.cpp provides its own main(), so link plain gtest, not gtest_main
target_link_libraries(FileBrowserClientTest PRIVATE
    fb_client_core
    GTest::gtest
)

fb_system_links(FileBrowserClientTest)

add_test(NAME FileBrowserClientTest COMMAND FileBrowserClientTest)

# The test reads android_files.bin / files.bin from the current working directory
# (relative paths), so those data files must be present in the test working directory.
set_tests_properties(FileBrowserClientTest PROPERTIES
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
)
