add_executable(mloptc main.cpp)

target_link_libraries(mloptc
  PRIVATE mloptc_core project_warnings)
