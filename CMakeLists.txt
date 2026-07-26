cmake_minimum_required(VERSION 3.18)
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()
project(BMT)

find_package(Boost REQUIRED COMPONENTS program_options)
find_package(OpenSSL QUIET)
find_package(libzip CONFIG QUIET)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-libplist CONFIG QUIET)

if(NOT TARGET OpenSSL::Crypto OR
   NOT TARGET libzip::zip OR
   NOT TARGET unofficial::libplist::libplist)
    find_package(PkgConfig REQUIRED)
endif()

if(TARGET OpenSSL::Crypto)
    set(BMT_CRYPTO_TARGET OpenSSL::Crypto)
else()
    pkg_check_modules(LIBCRYPTO REQUIRED IMPORTED_TARGET libcrypto)
    set(BMT_CRYPTO_TARGET PkgConfig::LIBCRYPTO)
endif()

if(TARGET libzip::zip)
    set(BMT_LIBZIP_TARGET libzip::zip)
else()
    pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip)
    set(BMT_LIBZIP_TARGET PkgConfig::LIBZIP)
endif()

if(TARGET unofficial::libplist::libplist)
    set(BMT_LIBPLIST_TARGET unofficial::libplist::libplist)
else()
    pkg_check_modules(LIBPLIST REQUIRED IMPORTED_TARGET libplist-2.0)
    set(BMT_LIBPLIST_TARGET PkgConfig::LIBPLIST)
endif()

aux_source_directory("${CMAKE_CURRENT_LIST_DIR}/lib" BMT_LIB_SRCS)
add_library(BMT STATIC "${BMT_LIB_SRCS}")
target_compile_features(BMT PUBLIC cxx_std_20)
target_include_directories(BMT PUBLIC "${CMAKE_CURRENT_LIST_DIR}/include")
target_link_libraries(BMT PUBLIC
    ${BMT_LIBZIP_TARGET}
    ${BMT_LIBPLIST_TARGET}
    ${BMT_CRYPTO_TARGET})
target_link_libraries(BMT PRIVATE nlohmann_json::nlohmann_json)

add_executable(BemaniTools tools/BemaniTools.cpp)
target_link_libraries(BemaniTools PRIVATE BMT Boost::program_options)

include(GNUInstallDirs)
install(TARGETS BemaniTools RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES README.md DESTINATION "${CMAKE_INSTALL_DOCDIR}")

include(CTest)
if(BUILD_TESTING)
    add_executable(BMTTests tests/BMTTests.cpp)
    target_link_libraries(BMTTests PRIVATE BMT)
    add_test(NAME BMTTests COMMAND BMTTests)
    add_test(NAME BMTCLIHelp COMMAND BemaniTools --help)
    add_test(NAME BMTCLIBFCodecHelp COMMAND BemaniTools bfcodec decrypt --help)
    add_test(NAME BMTCLIMarkerHelp COMMAND BemaniTools marker build --help)
    add_test(NAME BMTCLIRejectsUnknown COMMAND BemaniTools unknown command)
    set_tests_properties(BMTCLIRejectsUnknown PROPERTIES WILL_FAIL TRUE)
    add_test(NAME BMTCLIRejectsMissingRequired COMMAND BemaniTools mulist decrypt)
    set_tests_properties(BMTCLIRejectsMissingRequired PROPERTIES WILL_FAIL TRUE)
    add_test(NAME BMTCLIRejectsMarkerListEncrypt COMMAND BemaniTools marker-list encrypt)
    set_tests_properties(BMTCLIRejectsMarkerListEncrypt PROPERTIES WILL_FAIL TRUE)
endif()
