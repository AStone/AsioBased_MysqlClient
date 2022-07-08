/*

 */
#pragma once

#include <assert.h>
#include <mariadb/mysql.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <memory>
#include <string>

namespace db {
enum class SqlStatus {
    Ok,
    End
};
class MysqlResult {
   public:
    using RowSizeType = unsigned long;
    using FieldSizeType = unsigned long;
    using SizeType = std::size_t;
    using FieldNames = std::map<std::string, RowSizeType>;
    MysqlResult(const std::shared_ptr<MYSQL_RES>& r, SizeType affected_rows, unsigned long long insert_id) : result_ptr_(r),
                                                                                                             rows_number_(r ? mysql_num_rows(r.get()) : 0),
                                                                                                             field_array_(r ? mysql_fetch_fields(r.get()) : nullptr),
                                                                                                             fields_number_(r ? mysql_num_fields(r.get()) : 0),
                                                                                                             affected_rows_(affected_rows),
                                                                                                             insert_id_(insert_id) {
        if (fields_number_ > 0) {
            fields_map_ptr_ = std::make_shared<FieldNames>();
            for (unsigned long i = 0; i < fields_number_; ++i) {
                std::string field_name = field_array_[i].name;
                std::transform(field_name.begin(), field_name.end(), field_name.begin(), [](unsigned char c) { return tolower(c); });
                (*fields_map_ptr_)[field_name] = i;
            }
        }
        if (rows_number_ > 0) {
            rows_ptr_ = std::make_shared<std::vector<std::pair<char**, std::vector<unsigned long>>>>();
            MYSQL_ROW row;
            std::vector<unsigned long> vLens;
            vLens.resize(fields_number_);
            while ((row = mysql_fetch_row(r.get())) != NULL) {
                auto lengths = mysql_fetch_lengths(r.get());
                memcpy(vLens.data(),
                       lengths,
                       sizeof(unsigned long) * fields_number_);
                rows_ptr_->push_back(std::make_pair(row, vLens));
            }
        }
    }
    /**
     * @brief 结果的行数
     *
     * @return SizeType
     */
    SizeType size() const noexcept { return rows_number_; }

    /**
     * @brief 一行有多少字段
     *
     * @return RowSizeType
     */
    RowSizeType columns() const noexcept { return fields_number_; }

    /**
     * @brief 第number段的字段名字
     *
     * @param number
     * @return const char*
     */
    const char* columnName(RowSizeType number) const {
        assert(number < fields_number_);
        if (field_array_) return field_array_[number].name;
        return "";
    }

    /**
     * @brief update等语句的影响行数
     *
     * @return SizeType
     */
    SizeType affectedRows() const noexcept {
        return affected_rows_;
    }

    /**
     * @brief 根据字段名字查找是第几列
     *
     * @param colName
     * @return RowSizeType
     */
    RowSizeType columnNumber(const char colName[]) const {
        if (!fields_map_ptr_) return -1;
        std::string col(colName);
        std::transform(col.begin(), col.end(), col.begin(), [](unsigned char c) {
            return tolower(c);
        });
        if (fields_map_ptr_->find(col) != fields_map_ptr_->end())
            return (*fields_map_ptr_)[col];
        return -1;
    }

    /**
     * @brief 根据row和col得到格子的值
     *
     * @param row
     * @param column
     * @return const char*
     */
    const char* getValue(SizeType row,
                         RowSizeType column) const {
        if (rows_number_ == 0 || fields_number_ == 0)
            return NULL;
        assert(row < rows_number_);
        assert(column < fields_number_);
        return (*rows_ptr_)[row].first[column];
    }

    /**
     * @brief 根据col和row得到这个格子的值的长度
     *
     * @param row
     * @param column
     * @return FieldSizeType
     */
    FieldSizeType getLength(SizeType row,
                            RowSizeType column) const {
        if (rows_number_ == 0 || fields_number_ == 0)
            return 0;
        assert(row < rows_number_);
        assert(column < fields_number_);
        return (*rows_ptr_)[row].second[column];
    }

    bool isNull(SizeType row, RowSizeType column) const { return getValue(row, column) == NULL; }
    unsigned long long insertId() const noexcept { return insert_id_; }

   private:
    const std::shared_ptr<MYSQL_RES> result_ptr_;  //保存mql_res

    std::shared_ptr<FieldNames> fields_map_ptr_;  //字段名字和对应的列数的映射
    std::shared_ptr<std::vector<std::pair<char**, std::vector<unsigned long>>>> rows_ptr_;
    const SizeType rows_number_;

    const MYSQL_FIELD* field_array_;  //保存字段
    const FieldSizeType fields_number_;

    const SizeType affected_rows_;
    const unsigned long long insert_id_;

   private:
    // enum Field::DataTypes convertNativeType(enum_field_types mysqlType) const;
};
using MysqlResultPtr = std::shared_ptr<MysqlResult>;

}  // namespace db