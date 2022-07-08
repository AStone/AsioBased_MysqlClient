# AisoBased_MysqlClient
基于Asio与协程的Mysql异步客户端
## Description
一个基于Asio提供的CoroutineTs框架实现的Mysql异步客户端,项目依赖asio,mariadb以及gcc11, 我自己使用的版本如下:
* Asio 1.21 
* MariaDB 10.8.3 (需要使用mariaDb提供的非阻塞api)
* gcc 11.2.1 (需要支持c++20 CoroutineTs)
## Feature
* 使用异步.非阻塞IO的方式访问数据库,最大化客户端性能
* 使用协程,可以以同步的方式编写异步程序,简化了编写难度
* 使用连接池的方式连接数据库,并支持动态扩容
* 支持MySql事务,使用方式可见 example 中的 test.hpp
## 测试流程
在 example 提供了一个简单的测试函数,需要手动修改MySQL的登陆相关信息以及测试的sql语句,修改完成后,跳转到CMakeLists.txt所在目录,执行如下命令
* mkdir build; cd build; cmake ..;make;
