add_library(anyps5_launcher_core STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/LauncherCore.cpp
)
target_include_directories(anyps5_launcher_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(WIN32)
    add_executable(anyps5_launcher WIN32 ${CMAKE_CURRENT_SOURCE_DIR}/src/Main.cpp)
else()
    add_executable(anyps5_launcher ${CMAKE_CURRENT_SOURCE_DIR}/src/Main.cpp)
endif()
target_link_libraries(anyps5_launcher PRIVATE anyps5_launcher_core SDL2-static)
if(TARGET SDL2main)
    target_link_libraries(anyps5_launcher PRIVATE SDL2main)
endif()
if(WIN32)
    target_link_libraries(anyps5_launcher PRIVATE comdlg32 shell32 ole32)
endif()
set_target_properties(anyps5_launcher PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/launcher")
add_dependencies(anyps5_launcher relinker)

set(anyps5RuntimeDirectory "")
if(WIN32)
    get_filename_component(anyps5RuntimeDirectory "${CMAKE_CXX_COMPILER}" DIRECTORY)
endif()

add_custom_target(launcher_bundle
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:relinker>" "$<TARGET_FILE_DIR:anyps5_launcher>"
    COMMAND ${CMAKE_COMMAND}
        -DSOURCE=${CMAKE_BINARY_DIR}/core/libs/libs
        -DDESTINATION=$<TARGET_FILE_DIR:anyps5_launcher>/libs
        -DRUNTIME_DIRECTORY=${anyps5RuntimeDirectory}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/CopyLibraries.cmake
    DEPENDS anyps5_launcher relinker libs
    VERBATIM
)

if(BUILD_TESTING)
    add_test_executable(launcher_core_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/LauncherCoreTests.cpp)
    target_link_libraries(launcher_core_tests PRIVATE anyps5_launcher_core)
    add_test(NAME launcher_core COMMAND launcher_core_tests)
    add_test_executable(launcher_install_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/LauncherInstallTests.cpp)
    target_link_libraries(launcher_install_tests PRIVATE anyps5_launcher_core)
    add_test(NAME launcher_install COMMAND launcher_install_tests)
    add_test(NAME launcher_smoke COMMAND anyps5_launcher --smoke-test)
    set_tests_properties(launcher_smoke PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")
endif()
