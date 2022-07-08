cmake_minimum_required(VERSION 3.20)
set(CMAKE_C_COMPILER /usr/bin/gcc)
set(CMAKE_CXX_COMPILER /usr/local/bin/g++)

project(TEST)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++2a -g -gdwarf-3 -Wall -O0 -Wno-unused-variable -pthread -fcoroutines")
find_library(MARIA_LIB libmariadbclient.so usr/lib/x86_64-linux-gnu)
#如果你的数据库是mariadb，你还需要安装mariadb-devel库；如果你的数据库是mysql，你需要安装mysql-devel，否则会出现如下错误提示。
IF (NOT MARIA_LIB)
    MESSAGE(FATAL_ERROR "mysqlclient not found. If your mysql is mariadb, please install mariadb-devel otherwise install mysql-devel.")
ENDIF(NOT MARIA_LIB)

include_directories(/usr/include /usr/local/include include)
add_executable(main main.cpp)
target_link_libraries(main mariadbclient)