#pragma once

#include "mysql_client.hpp"
using namespace std::chrono_literals;
namespace test {
static void mysql_test() {
    const char* usr_name = "test";
    const char* host = "127.0.0.1";
    const char* port = "3306";
    const char* password = "";
    const char* database = "mysql";
    const char* character_set = "";
    const char* sql = "select user,host from user";

    {
        std::cout << "Original api test begin:\n";
        MYSQL* mysql = new MYSQL;
        mysql_init(mysql);
        mysql_real_connect(mysql, host, usr_name, password, database, 3306, NULL, 0);

        mysql_real_query(mysql, sql, strlen(sql));
        MYSQL_RES* res = new MYSQL_RES;
        res = mysql_store_result(mysql);
        mysql_close(mysql);
        std::cout << "Original api test end.\n";
    }

    {
        std::cout << "Single sql test begin:\n";
        int min_conn_size(2), max_conn_size(4);
        auto client_ptr = std::make_shared<db::MysqlClient>(db::ConnectionInfo(usr_name, host, port, password, database, character_set), min_conn_size, min_conn_size);
        client_ptr->init();
        std::this_thread::sleep_for(1s);
        client_ptr->query(sql, [](db::MysqlResultPtr ptr) {
            std::cout << " this is single sql :\n";
            for (size_t i = 0; i < ptr->size(); i++) {
                for (size_t j = 0; j < ptr->columns(); ++j) {
                    std::cout << ptr->getValue(i, j) << "\t";
                }
                std::cout << "\n";
            }
        });
        std::cout << "Single sql test end\n";

        {
            std::cout << "Transaction test begin:\n";
            auto trans_ptr = client_ptr->new_transaction([](bool ret) {
                if (ret) {
                    std::cout << "commit success\n";
                } else {
                    std::cout << "commit failed\n";
                }
            });
            trans_ptr->execute_sql(
                sql, [](db::MysqlResultPtr ptr) {
                    std::cout <<" this is transaction :\n";
                     for (size_t i = 0; i < ptr->size(); i++) {
                         for (size_t j = 0; j < ptr->columns(); ++j) {
                             std::cout << ptr->getValue(i, j) << "\t";
                         }
                         std::cout << "\n";
                     } }, [](std::exception_ptr ec) {
                    try{
                        if(ec){
                            std::rethrow_exception(ec);
                        }
                    }catch(std::exception& e){
                        std::cout << e.what() << "\n";
                    } });
            std::cout << "Transaction test end:\n";
        }
        client_ptr->join();
    }
}

}  // namespace test