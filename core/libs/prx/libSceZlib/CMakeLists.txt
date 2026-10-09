set(ZLIB_SOURCE_DIR ${PROJECT_SOURCE_DIR}/3rdparty/libjpeg-turbo/src/spng/zlib)
enable_language(C)

add_library(sce_zlib_backend STATIC
    ${ZLIB_SOURCE_DIR}/adler32.c
    ${ZLIB_SOURCE_DIR}/crc32.c
    ${ZLIB_SOURCE_DIR}/inffast.c
    ${ZLIB_SOURCE_DIR}/inflate.c
    ${ZLIB_SOURCE_DIR}/inftrees.c
    ${ZLIB_SOURCE_DIR}/zutil.c
)
target_include_directories(sce_zlib_backend PUBLIC ${ZLIB_SOURCE_DIR})
set_target_properties(sce_zlib_backend PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(libSceZlib SHARED EXCLUDE_FROM_ALL Export.cpp)
target_include_directories(libSceZlib PRIVATE ${LIBS_INCLUDE_DIR})
target_link_libraries(libSceZlib PRIVATE sce_zlib_backend)
configure_windows_unwind(libSceZlib)

if(BUILD_TESTING)
    add_test_executable(guest_zlib_tests ${LIBS_INCLUDE_DIR}/tests/GuestZlib.cpp)
    target_include_directories(guest_zlib_tests PRIVATE ${LIBS_INCLUDE_DIR})
    target_link_libraries(guest_zlib_tests PRIVATE libSceZlib)
    configure_windows_unwind(guest_zlib_tests)
    add_test(NAME guest_zlib COMMAND guest_zlib_tests)
    if(WIN32)
        set_tests_properties(guest_zlib PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:libSceZlib>")
    endif()
endif()
