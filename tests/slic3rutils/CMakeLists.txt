get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)
add_executable(${_TEST_NAME}_tests
    ${_TEST_NAME}_tests_main.cpp
    test_dev_mapping.cpp
    test_filament_bitmap_utils.cpp
    test_network_versions.cpp
    test_plugin_capability_config.cpp
    test_plugin_config.cpp
    test_plugin_capabilities_in_use.cpp
    test_plugin_install.cpp
    test_plugin_lifecycle.cpp
    test_slicing_pipeline_bindings.cpp
    test_slicing_pipeline_config.cpp
    test_plugin_sort.cpp
    test_plugin_cloud_metadata.cpp
    ../fff_print/test_helpers.cpp
    )
if (MSVC)
    target_link_libraries(${_TEST_NAME}_tests Setupapi.lib)
endif ()


target_link_libraries(${_TEST_NAME}_tests test_common libslic3r_gui libslic3r Catch2::Catch2WithMain)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")
if(ORCA_JVM_AVAILABLE)
  find_package(JNI QUIET)
  if(JNI_FOUND)
    target_include_directories(${_TEST_NAME}_tests PRIVATE ${JNI_INCLUDE_DIRS})
  endif()
endif()

orcaslicer_copy_test_dlls()

add_executable(install_demo C:/tmp/install_demo.cpp)
target_link_libraries(install_demo test_common libslic3r_gui libslic3r)
if(ORCA_JVM_AVAILABLE)
  find_package(JNI QUIET)
  if(JNI_FOUND)
    target_include_directories(install_demo PRIVATE ${JNI_INCLUDE_DIRS})
  endif()
endif()
set_property(TARGET install_demo PROPERTY FOLDER "tests")
if(TARGET orca_hook_examples)
  add_dependencies(install_demo orca_hook_examples)
endif()
