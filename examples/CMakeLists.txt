add_executable(cpct-example basic_usage.cpp)

target_link_libraries(cpct-example
  PRIVATE
    libcompress
)

target_compile_definitions(cpct-example PRIVATE _CRT_SECURE_NO_WARNINGS)
