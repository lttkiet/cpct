add_executable(cpct main.cpp)

target_link_libraries(cpct
  PRIVATE
    libcompress
    fmt::fmt
    cxxopts::cxxopts
)

target_compile_definitions(cpct PRIVATE _CRT_SECURE_NO_WARNINGS)
