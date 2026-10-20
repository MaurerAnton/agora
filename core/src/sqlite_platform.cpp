#include "agora/core/sqlite_platform.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace agora::core::sqlite_platform {

SqlValue SqlValue::null() {
    return SqlValue{};
}

SqlValue SqlValue::text_value(std::string value) {
    SqlValue out;
    out.kind = Kind::Text;
    out.text = std::move(value);
    return out;
}

SqlValue SqlValue::long_value(long long value) {
    SqlValue out;
    out.kind = Kind::Long;
    out.number = value;
    return out;
}

SqlValue SqlValue::int_value(int value) {
    SqlValue out;
    out.kind = Kind::Int;
    out.number = value;
    return out;
}

int QueryResult::column_index(std::string_view name) const {
    for (std::size_t index = 0; index < columns.size(); ++index) {
        if (columns[index] == name) return static_cast<int>(index);
    }
    throw MigrationException{"IllegalArgumentException",
                             "column '" + std::string(name) + "' does not exist"};
}

}  // namespace agora::core::sqlite_platform
