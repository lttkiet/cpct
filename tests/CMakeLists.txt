add_executable(cpct-test
  test_main.cpp
  test_types.cpp
  test_format_detection.cpp
  test_zip.cpp
  test_tar.cpp
  test_gzip.cpp
  test_bzip2.cpp
  test_xz.cpp
  test_zstd.cpp
  test_sevenz.cpp
  test_rar.cpp
  test_multipart.cpp
  test_advanced.cpp
)

target_link_libraries(cpct-test
  PRIVATE
    libcompress
    GTest::gtest
    GTest::gtest_main
)

target_compile_definitions(cpct-test PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME cpct-test COMMAND cpct-test)
