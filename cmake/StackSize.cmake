# Enlarges the stack of our executables.
#
# Why this is needed: the AST interpreter executes a ToyC function call as
# native C++ recursion, so one ToyC frame costs several host frames. Windows
# gives an executable a 1 MB stack by default, which is not enough to reach the
# spec's call-depth limit -- the process would die with a stack overflow before
# the limit check could report a clean runtime error.
#
# A crash and a diagnosed runtime error are very different things for this
# project: the differential tester has to tell "this program legitimately
# exceeded the depth limit" apart from "the compiler fell over", and it can
# only do that if exceeding the limit is always the diagnosed outcome.
#
# On Linux the default stack is already 8 MB, so nothing is needed there.
function(mloptc_set_stack_size target)
  if(WIN32)
    if(MSVC)
      target_link_options(${target} PRIVATE /STACK:33554432) # 32 MB
    else()
      # clang with the GNU-style driver still uses the MSVC linker underneath.
      target_link_options(${target} PRIVATE -Xlinker /STACK:33554432)
    endif()
  endif()
endfunction()
