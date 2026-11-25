cmake_minimum_required(VERSION 3.16)

project(PriceMonitor VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CURL REQUIRED)

# 注意：Drogon 的 CMake 配置会把自身目录加入 CMAKE_MODULE_PATH，
# 导致 find_package(SQLite3) 命中 Drogon 自带的旧版 FindSQLite3.cmake，
# 该模块只创建 SQLite3_lib，不会创建 SQLite::SQLite3，最终链接时缺少 -lsqlite3。
# 这里直接探测头文件和库，避免依赖被污染的模块实现。
find_library(PM_SQLITE3_LIBRARY NAMES sqlite3)
find_path(PM_SQLITE3_INCLUDE_DIR NAMES sqlite3.h)
if(NOT PM_SQLITE3_LIBRARY OR NOT PM_SQLITE3_INCLUDE_DIR)
    message(FATAL_ERROR "未找到 SQLite3，请安装：sudo apt-get install -y libsqlite3-dev")
endif()

# Lexbor 官方仓库默认不提供 pkg-config 文件，这里同时支持 pkg-config 与手工探测。
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LEXBOR QUIET IMPORTED_TARGET lexbor)
endif()

file(GLOB_RECURSE PM_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc)

add_executable(PriceMonitor ${PM_SOURCES})

target_include_directories(PriceMonitor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${PM_SQLITE3_INCLUDE_DIR}
)
target_link_libraries(PriceMonitor PRIVATE
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    CURL::libcurl
    ${PM_SQLITE3_LIBRARY}
)

if(TARGET PkgConfig::LEXBOR)
    target_link_libraries(PriceMonitor PRIVATE PkgConfig::LEXBOR)
else()
    find_path(LEXBOR_INCLUDE_DIR NAMES lexbor/html/html.h)
    find_library(LEXBOR_LIBRARY NAMES lexbor)
    if(NOT LEXBOR_INCLUDE_DIR OR NOT LEXBOR_LIBRARY)
        message(FATAL_ERROR
            "未找到 Lexbor。请安装 liblexbor-dev，或从源码构建并执行 make install：\n"
            "  git clone --depth 1 https://github.com/lexbor/lexbor.git\n"
            "  cd lexbor && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release\n"
            "  cmake --build build -j$(nproc) && sudo cmake --install build")
    endif()
    target_include_directories(PriceMonitor PRIVATE ${LEXBOR_INCLUDE_DIR})
    target_link_libraries(PriceMonitor PRIVATE ${LEXBOR_LIBRARY})
endif()

# 控制器由静态注册表自动注册，必须确保目标文件被链接器保留。
if(COMMAND drogon_create_controllers)
    drogon_create_controllers(PriceMonitor)
endif()

# GCC 8（如树莓派 Buster）需要显式链接 stdc++fs 才能使用 std::filesystem。
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.1)
    target_link_libraries(PriceMonitor PRIVATE stdc++fs)
endif()

install(TARGETS PriceMonitor RUNTIME DESTINATION bin)
install(DIRECTORY web/ DESTINATION share/pricemonitor/web)
install(DIRECTORY config/ DESTINATION share/pricemonitor/config)
