# AddressSanitizer / UndefinedBehaviorSanitizer configuration.
#
# IMPORTANT -- why this does NOT use the INTERFACE-target trick that
# cmake/CompilerWarnings.cmake uses:
#
#   Warnings are a per-TARGET concern. We want -Werror on our code and not on
#   GoogleTest, so an INTERFACE target that only our targets link is exactly
#   right.
#
#   Sanitizers are a per-PROGRAM concern. ASan changes the memory layout of
#   standard library containers (MSVC's STL adds annotation fields to
#   std::string and std::vector). If one translation unit is instrumented and
#   another is not, the two disagree about what std::string looks like -- an ODR
#   violation. MSVC's linker detects this and refuses:
#
#     lld-link: error: /failifmismatch: mismatch detected for 'annotate_string'
#
#   So the sanitizer flags must apply to EVERYTHING in the build, including
#   dependencies fetched by FetchContent. That means global flags, set here
#   before any add_subdirectory() call.
#
# Platform note: UBSan's runtime is incomplete on Windows, so the Windows build
# enables ASan only. The Linux CI job is the authoritative full ASan+UBSan run.

if(MLOPTC_ENABLE_SANITIZERS)
  if(WIN32)
    # ASan on Windows is incompatible with the DEBUG C runtime. Linking against
    # ucrtbased.dll produces a bad-free abort inside CRT startup, before main()
    # is ever reached -- a failure with a 24-frame stack trace that contains not
    # one line of our own code. Forcing the release CRT fixes it, and costs us
    # nothing: the debug heap's checks are a strict subset of what ASan does.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)
  endif()

  if(MSVC)
    # clang-cl / MSVC driver syntax.
    add_compile_options(/fsanitize=address)
  elseif(WIN32)
    # clang with the GNU-style driver, targeting windows-msvc.
    # No UBSan here: see the platform note above.
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
  else()
    set(MLOPTC_SAN_FLAGS
        -fsanitize=address,undefined
        -fno-omit-frame-pointer   # keeps stack traces readable
        -fno-sanitize-recover=all # abort on the first UB, do not warn and continue
    )
    add_compile_options(${MLOPTC_SAN_FLAGS})
    add_link_options(${MLOPTC_SAN_FLAGS})
  endif()
endif()

# Kept so targets can link it unconditionally. It carries no flags -- the
# sanitizer settings above are global, for the reason explained at the top.
add_library(project_sanitizers INTERFACE)
