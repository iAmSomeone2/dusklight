# ThreadSanitizer configuration

include(CMakeDependentOption)

set(SUPPORTED_COMPILERS "Clang" "GNU" "AppleClang")
set(TSAN_IGNORE_LIST "${CMAKE_CURRENT_LIST_DIR}/tsan-ignore.txt")
set(HAS_TSAN_SUPPORT CMAKE_CXX_COMPILER_ID IN_LIST SUPPORTED_COMPILERS)

cmake_dependent_option(ENABLE_TSAN "Enable ThreadSanitizer for detecting data races" OFF HAS_TSAN_SUPPORT OFF)

if (ENABLE_TSAN)
    message(STATUS "dusklight: ThreadSanitizer is enabled. Expect slower performance and increased memory usage. This is intended for debugging purposes only.")
    # Clang and GCC support ThreadSanitizer with the same set of flags.
    # -O2 is used to enable optimizations that are compatible with ThreadSanitizer, while still providing useful debugging information. Running with TSan is too slow otherwise.
    add_compile_options("-fsanitize=thread" "-fno-omit-frame-pointer" "-g" "-O2")
    link_libraries("-fsanitize=thread")

    # Currently, only Clang supports the ignore list feature.
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang$")
        if (IS_READABLE "${TSAN_IGNORE_LIST}")
            message(STATUS "dusklight: Using ThreadSanitizer ignore list: ${TSAN_IGNORE_LIST}")
            add_compile_options("-fsanitize-ignorelist=${TSAN_IGNORE_LIST}")
        else ()
            message(WARNING "dusklight: ThreadSanitizer ignore list not found: ${TSAN_IGNORE_LIST}. This may result in false positives.")
        endif ()
    else ()
        message(WARNING "dusklight: ThreadSanitizer ignore list is not supported for this compiler: ${CMAKE_CXX_COMPILER_ID}. This may result in false positives.")
    endif ()
endif ()
