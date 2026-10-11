# Same INTERFACE-target trick as the warnings: only our own code is instrumented.
#
# Platform note: ASan works with clang on Windows (it needs the MSVC runtime),
# but UBSan's runtime support on Windows is incomplete. We therefore treat the
# Linux CI job as the authoritative sanitizer run and keep Windows as the fast
# local iteration loop. This is recorded in docs/DESIGN.md.
add_library(project_sanitizers INTERFACE)

if(MLOPTC_ENABLE_SANITIZERS)
  if(MSVC)
    target_compile_options(project_sanitizers INTERFACE /fsanitize=address)
  else()
    set(MLOPTC_SAN_FLAGS
      -fsanitize=address,undefined
      -fno-omit-frame-pointer      # keeps stack traces readable
      -fno-sanitize-recover=all)   # abort on first UB rather than warn and continue
    target_compile_options(project_sanitizers INTERFACE ${MLOPTC_SAN_FLAGS})
    target_link_options(project_sanitizers INTERFACE ${MLOPTC_SAN_FLAGS})
  endif()
endif()
