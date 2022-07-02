# AisoBased_MysqlClient
基于Asio与协程的Mysql异步客户端
## Description
一个基于Asio提供的CoroutineTs框架实现的Mysql异步客户端,项目依赖asio,mariadb以及gcc11, 我自己使用的版本如下:
* Asio 1.21 
* MariaDB 10.8.3 (需要使用mariaDb提供的非阻塞api)
* gcc 11.2.1 (需要支持c++20 CoroutineTs)
## Feature
项目的优点
* 使用异步.非阻塞IO的方式访问数据库,最大化客户端性能
* 使用协程,可以以同步的方式编写异步程序,简化了编写难度
* 使用连接池的方式连接数据库,省去了建立Tcp的过程
项目的不足:
* Asio提供的无栈协程相比异步回调多了许多的堆栈操作,性能上不如纯异步
* 由于结果是异步得到的,因此上层的业务逻辑也需要调整成异步方式,如增加一个异步队列等
