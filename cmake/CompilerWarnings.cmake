# An INTERFACE library carries no code -- it exists only to hand flags to the
# targets that link it. We attach warnings this way (instead of setting
# CMAKE_CXX_FLAGS globally) so that third-party code such as GoogleTest is NOT
# compiled with -Werror. Otherwise one warning inside a dependency we do not
# control would break our build.
add_library(project_warnings INTERFACE)

set(MLOPTC_GCC_CLANG_WARNINGS
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow              # a local hiding an outer name: a classic compiler-writer bug
  -Wnon-virtual-dtor    # deleting a derived object through a base pointer
  -Wold-style-cast
  -Wcast-align
  -Wunused
  -Woverloaded-virtual  # a derived method that hides rather than overrides
  -Wconversion          # implicit narrowing, e.g. size_t -> int
  -Wsign-conversion
  -Wdouble-promotion
  -Wformat=2
  -Wimplicit-fallthrough # a switch case falling through without [[fallthrough]]
  -Wswitch              # <-- the highest-value warning in this project: when we
                        #     add an IR opcode, every switch that forgot it fails
)

set(MLOPTC_MSVC_WARNINGS /W4 /permissive- /w14640)

if(MSVC)
  set(MLOPTC_WARNINGS ${MLOPTC_MSVC_WARNINGS})
  if(MLOPTC_WARNINGS_AS_ERRORS)
    list(APPEND MLOPTC_WARNINGS /WX)
  endif()
else()
  set(MLOPTC_WARNINGS ${MLOPTC_GCC_CLANG_WARNINGS})
  if(MLOPTC_WARNINGS_AS_ERRORS)
    list(APPEND MLOPTC_WARNINGS -Werror)
  endif()
endif()

target_compile_options(project_warnings INTERFACE ${MLOPTC_WARNINGS})
