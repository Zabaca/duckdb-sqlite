#include "sqlite_stmt.hpp"
#include "sqlite_db.hpp"
#include "sqlite_scanner.hpp"

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/uuid.hpp"

#include <cstdint>
#include <limits>

// PoC: libsql-probe C ABI subset used for statements.
extern "C" {
int         lp_step(::lp_stmt *s);
int         lp_column_count(::lp_stmt *s);
const char *lp_column_name(::lp_stmt *s, int idx);
int         lp_column_type(::lp_stmt *s, int idx);
int64_t     lp_column_int64(::lp_stmt *s, int idx);
double      lp_column_double(::lp_stmt *s, int idx);
const char *lp_column_text(::lp_stmt *s, int idx);
int         lp_column_bytes(::lp_stmt *s, int idx);
const unsigned char *lp_column_blob(::lp_stmt *s, int idx);
void        lp_finalize(::lp_stmt *s);
int         lp_reset(::lp_stmt *s);
int         lp_clear_bindings(::lp_stmt *s);
int         lp_bind_int64(::lp_stmt *s, int idx, int64_t v);
int         lp_bind_double(::lp_stmt *s, int idx, double v);
int         lp_bind_null(::lp_stmt *s, int idx);
int         lp_bind_text(::lp_stmt *s, int idx, const char *text, int len);
int         lp_bind_blob(::lp_stmt *s, int idx, const unsigned char *data, int len);
}

#define LP_SQLITE_ROW  100
#define LP_SQLITE_DONE 101

namespace duckdb {

SQLiteStatement::SQLiteStatement() : db(nullptr), stmt(nullptr) {
}

SQLiteStatement::SQLiteStatement(sqlite3 *db, sqlite3_stmt *stmt) : db(db), stmt(stmt) {
	D_ASSERT(db);
}

SQLiteStatement::~SQLiteStatement() {
	Close();
}

SQLiteStatement::SQLiteStatement(SQLiteStatement &&other) noexcept {
	std::swap(db, other.db);
	std::swap(stmt, other.stmt);
	std::swap(libsql_stmt, other.libsql_stmt);
	std::swap(owner_db, other.owner_db);
}

SQLiteStatement &SQLiteStatement::operator=(SQLiteStatement &&other) noexcept {
	std::swap(db, other.db);
	std::swap(stmt, other.stmt);
	std::swap(libsql_stmt, other.libsql_stmt);
	std::swap(owner_db, other.owner_db);
	return *this;
}

int SQLiteStatement::Step() {
	if (IsLibSQL()) {
		auto rc = lp_step(libsql_stmt);
		if (rc == LP_SQLITE_ROW) {
			return true;
		}
		if (rc == LP_SQLITE_DONE) {
			return false;
		}
		throw std::runtime_error("libsql lp_step failed");
	}
	D_ASSERT(db);
	D_ASSERT(stmt);
	auto rc = sqlite3_step(stmt);
	if (rc == SQLITE_ROW) {
		return true;
	}
	if (rc == SQLITE_DONE) {
		return false;
	}
	throw std::runtime_error(string(sqlite3_errmsg(db)));
}
int SQLiteStatement::GetType(idx_t col) {
	if (IsLibSQL()) {
		return lp_column_type(libsql_stmt, int(col));
	}
	D_ASSERT(stmt);
	return sqlite3_column_type(stmt, col);
}

string SQLiteStatement::GetName(idx_t col) {
	if (IsLibSQL()) {
		auto *nm = lp_column_name(libsql_stmt, int(col));
		return nm ? string(nm) : string();
	}
	D_ASSERT(stmt);
	return sqlite3_column_name(stmt, col);
}

idx_t SQLiteStatement::GetColumnCount() {
	if (IsLibSQL()) {
		return idx_t(lp_column_count(libsql_stmt));
	}
	D_ASSERT(stmt);
	return sqlite3_column_count(stmt);
}

bool SQLiteStatement::IsOpen() {
	return stmt || libsql_stmt;
}

void SQLiteStatement::Close() {
	if (!IsOpen()) {
		return;
	}
	if (libsql_stmt) {
		lp_finalize(libsql_stmt);
		libsql_stmt = nullptr;
		db = nullptr;
		owner_db = nullptr;
		return;
	}
	sqlite3_finalize(stmt);
	db = nullptr;
	stmt = nullptr;
}

void SQLiteStatement::CheckTypeMatches(const SqliteBindData &bind_data, sqlite3_value *val, int sqlite_column_type,
                                       int expected_type, idx_t col_idx) {
	D_ASSERT(stmt);
	if (bind_data.all_varchar) {
		// no type check required
		return;
	}
	if (sqlite_column_type != expected_type) {
		auto column_name = string(sqlite3_column_name(stmt, int(col_idx)));
		auto value_as_text = string((char *)sqlite3_value_text(val));
		auto message = "Invalid type in column \"" + column_name + "\": column was declared as " +
		               SQLiteUtils::TypeToString(expected_type) + ", found \"" + value_as_text + "\" of type \"" +
		               SQLiteUtils::TypeToString(sqlite_column_type) + "\" instead.";
		message += "\n* SET sqlite_all_varchar=true to load all columns as VARCHAR "
		           "and skip type conversions";
		throw Exception(ExceptionType::MISMATCH_TYPE, message);
	}
}

void SQLiteStatement::CheckTypeIsFloatOrInteger(sqlite3_value *val, int sqlite_column_type, idx_t col_idx) {
	if (sqlite_column_type != SQLITE_FLOAT && sqlite_column_type != SQLITE_INTEGER) {
		auto column_name = string(sqlite3_column_name(stmt, int(col_idx)));
		auto value_as_text = string((const char *)sqlite3_value_text(val));
		auto message = "Invalid type in column \"" + column_name + "\": expected float or integer, found \"" +
		               value_as_text + "\" of type \"" + SQLiteUtils::TypeToString(sqlite_column_type) + "\" instead.";
		message += "\n* SET sqlite_all_varchar=true to load all columns as VARCHAR "
		           "and skip type conversions";
		throw Exception(ExceptionType::MISMATCH_TYPE, message);
	}
}

void SQLiteStatement::Reset() {
	if (IsLibSQL()) {
		if (lp_reset(libsql_stmt) != 0) {
			throw std::runtime_error("libsql lp_reset failed");
		}
		return;
	}
	SQLiteUtils::Check(sqlite3_reset(stmt), db);
}

void SQLiteStatement::ClearBindings() {
	if (IsLibSQL()) {
		if (lp_clear_bindings(libsql_stmt) != 0) {
			throw std::runtime_error("libsql lp_clear_bindings failed");
		}
		return;
	}
	SQLiteUtils::Check(sqlite3_clear_bindings(stmt), db);
}

template <>
string SQLiteStatement::GetValue(idx_t col) {
	if (IsLibSQL()) {
		auto *ptr = lp_column_text(libsql_stmt, int(col));
		if (!ptr) {
			return string();
		}
		return string(ptr);
	}
	D_ASSERT(stmt);
	auto ptr = sqlite3_column_text(stmt, col);
	if (!ptr) {
		return string();
	}
	return string((char *)ptr);
}

template <>
int SQLiteStatement::GetValue(idx_t col) {
	if (IsLibSQL()) {
		return int(lp_column_int64(libsql_stmt, int(col)));
	}
	D_ASSERT(stmt);
	return sqlite3_column_int(stmt, col);
}

template <>
int64_t SQLiteStatement::GetValue(idx_t col) {
	if (IsLibSQL()) {
		return lp_column_int64(libsql_stmt, int(col));
	}
	D_ASSERT(stmt);
	return sqlite3_column_int64(stmt, col);
}

template <>
sqlite3_value *SQLiteStatement::GetValue(idx_t col) {
	// Callers on the libsql path must branch before reaching here — we can't
	// synthesize a sqlite3_value* from a libsql row. See sqlite_scanner.cpp
	// hot-loop flavor split.
	if (IsLibSQL()) {
		throw InternalException("sqlite3_value* GetValue not supported on libsql statement");
	}
	D_ASSERT(stmt);
	return sqlite3_column_value(stmt, col);
}

template <>
void SQLiteStatement::Bind(idx_t col, int32_t value) {
	if (IsLibSQL()) {
		if (lp_bind_int64(libsql_stmt, int(col + 1), int64_t(value)) != 0) {
			throw std::runtime_error("libsql lp_bind_int64 failed");
		}
		return;
	}
	SQLiteUtils::Check(sqlite3_bind_int(stmt, col + 1, value), db);
}

template <>
void SQLiteStatement::Bind(idx_t col, int64_t value) {
	if (IsLibSQL()) {
		if (lp_bind_int64(libsql_stmt, int(col + 1), value) != 0) {
			throw std::runtime_error("libsql lp_bind_int64 failed");
		}
		return;
	}
	SQLiteUtils::Check(sqlite3_bind_int64(stmt, col + 1, value), db);
}

template <>
void SQLiteStatement::Bind(idx_t col, double value) {
	if (IsLibSQL()) {
		if (lp_bind_double(libsql_stmt, int(col + 1), value) != 0) {
			throw std::runtime_error("libsql lp_bind_double failed");
		}
		return;
	}
	SQLiteUtils::Check(sqlite3_bind_double(stmt, col + 1, value), db);
}

void SQLiteStatement::BindBlob(idx_t col, const string_t &value) {
	if (IsLibSQL()) {
		if (lp_bind_blob(libsql_stmt, int(col + 1),
		                 reinterpret_cast<const unsigned char *>(value.GetDataUnsafe()),
		                 int(value.GetSize())) != 0) {
			throw std::runtime_error("libsql lp_bind_blob failed");
		}
		return;
	}
	SQLiteUtils::Check(sqlite3_bind_blob(stmt, col + 1, value.GetDataUnsafe(), value.GetSize(), nullptr), db);
}

void SQLiteStatement::BindBlob(idx_t col, const string &value) {
	if (IsLibSQL()) {
		if (lp_bind_blob(libsql_stmt, int(col + 1),
		                 reinterpret_cast<const unsigned char *>(value.c_str()),
		                 int(value.length())) != 0) {
			throw std::runtime_error("libsql lp_bind_blob failed");
		}
		return;
	}
	SQLiteUtils::Check(sqlite3_bind_blob(stmt, col + 1, value.c_str(), value.length(), nullptr), db);
}

void SQLiteStatement::BindText(idx_t col, const string_t &value) {
	if (IsLibSQL()) {
		if (lp_bind_text(libsql_stmt, int(col + 1), value.GetDataUnsafe(), int(value.GetSize())) != 0) {
			throw std::runtime_error("libsql lp_bind_text failed");
		}
		return;
	}
	SQLiteUtils::Check(sqlite3_bind_text(stmt, col + 1, value.GetDataUnsafe(), value.GetSize(), nullptr), db);
}

void SQLiteStatement::BindText(idx_t col, const string &value) {
	if (IsLibSQL()) {
		if (lp_bind_text(libsql_stmt, int(col + 1), value.c_str(), int(value.length())) != 0) {
			throw std::runtime_error("libsql lp_bind_text failed");
		}
		return;
	}
	SQLiteUtils::Check(sqlite3_bind_text(stmt, col + 1, value.c_str(), value.length(), nullptr), db);
}

template <>
void SQLiteStatement::Bind(idx_t col, std::nullptr_t value) {
	if (IsLibSQL()) {
		if (lp_bind_null(libsql_stmt, int(col + 1)) != 0) {
			throw std::runtime_error("libsql lp_bind_null failed");
		}
		return;
	}
	SQLiteUtils::Check(sqlite3_bind_null(stmt, col + 1), db);
}

void SQLiteStatement::BindValue(Vector &col, idx_t c, idx_t r) {
	auto &mask = FlatVector::Validity(col);
	if (!mask.RowIsValid(r)) {
		Bind<std::nullptr_t>(c, nullptr);
	} else {
		switch (col.GetType().id()) {
		case LogicalTypeId::BOOLEAN:
			Bind<int64_t>(c, FlatVector::GetData<bool>(col)[r] ? int64_t(1) : int64_t(0));
			break;
		case LogicalTypeId::TINYINT:
			Bind<int64_t>(c, int64_t(FlatVector::GetData<int8_t>(col)[r]));
			break;
		case LogicalTypeId::SMALLINT:
			Bind<int64_t>(c, int64_t(FlatVector::GetData<int16_t>(col)[r]));
			break;
		case LogicalTypeId::INTEGER:
			Bind<int64_t>(c, int64_t(FlatVector::GetData<int32_t>(col)[r]));
			break;
		case LogicalTypeId::BIGINT:
			Bind<int64_t>(c, FlatVector::GetData<int64_t>(col)[r]);
			break;
		case LogicalTypeId::UTINYINT:
			Bind<int64_t>(c, int64_t(FlatVector::GetData<uint8_t>(col)[r]));
			break;
		case LogicalTypeId::USMALLINT:
			Bind<int64_t>(c, int64_t(FlatVector::GetData<uint16_t>(col)[r]));
			break;
		case LogicalTypeId::UINTEGER:
			Bind<int64_t>(c, int64_t(FlatVector::GetData<uint32_t>(col)[r]));
			break;
		case LogicalTypeId::UBIGINT: {
			auto v = FlatVector::GetData<uint64_t>(col)[r];
			if (v > uint64_t(std::numeric_limits<int64_t>::max())) {
				throw NotImplementedException("UBIGINT value %llu overflows SQLite INTEGER (int64) on libsql bind",
				                              (unsigned long long)v);
			}
			Bind<int64_t>(c, int64_t(v));
			break;
		}
		case LogicalTypeId::HUGEINT:
			BindText(c, Hugeint::ToString(FlatVector::GetData<hugeint_t>(col)[r]));
			break;
		case LogicalTypeId::FLOAT:
			Bind<double>(c, double(FlatVector::GetData<float>(col)[r]));
			break;
		case LogicalTypeId::DOUBLE:
			Bind<double>(c, FlatVector::GetData<double>(col)[r]);
			break;
		case LogicalTypeId::UUID:
			// DuckDB UUID physical type is hugeint_t.
			BindText(c, UUID::ToString(FlatVector::GetData<hugeint_t>(col)[r]));
			break;
		case LogicalTypeId::DATE:
			BindText(c, Date::ToString(FlatVector::GetData<date_t>(col)[r]));
			break;
		case LogicalTypeId::TIME:
		case LogicalTypeId::TIME_TZ:
			BindText(c, Time::ToString(FlatVector::GetData<dtime_t>(col)[r]));
			break;
		case LogicalTypeId::TIMESTAMP:
		case LogicalTypeId::TIMESTAMP_TZ:
		case LogicalTypeId::TIMESTAMP_SEC:
		case LogicalTypeId::TIMESTAMP_MS:
		case LogicalTypeId::TIMESTAMP_NS:
			BindText(c, Timestamp::ToString(FlatVector::GetData<timestamp_t>(col)[r]));
			break;
		case LogicalTypeId::DECIMAL: {
			auto width = DecimalType::GetWidth(col.GetType());
			auto scale = DecimalType::GetScale(col.GetType());
			string s;
			switch (col.GetType().InternalType()) {
			case PhysicalType::INT16:
				s = Decimal::ToString(FlatVector::GetData<int16_t>(col)[r], width, scale);
				break;
			case PhysicalType::INT32:
				s = Decimal::ToString(FlatVector::GetData<int32_t>(col)[r], width, scale);
				break;
			case PhysicalType::INT64:
				s = Decimal::ToString(FlatVector::GetData<int64_t>(col)[r], width, scale);
				break;
			case PhysicalType::INT128:
				s = Decimal::ToString(FlatVector::GetData<hugeint_t>(col)[r], width, scale);
				break;
			default:
				throw InternalException("Unsupported DECIMAL internal type for SQLite::BindValue");
			}
			BindText(c, s);
			break;
		}
		case LogicalTypeId::BLOB:
			BindBlob(c, FlatVector::GetData<string_t>(col)[r]);
			break;
		case LogicalTypeId::VARCHAR:
			BindText(c, FlatVector::GetData<string_t>(col)[r]);
			break;
		default:
			throw InternalException("Unsupported type \"%s\" for SQLite::BindValue", col.GetType());
		}
	}
}

void SQLiteStatement::BindParameter(const Value &param, idx_t param_idx) {
	if (param.IsNull()) {
		Bind<std::nullptr_t>(param_idx, nullptr);
	} else {
		switch (param.type().id()) {
		case LogicalTypeId::BIGINT:
			Bind<int64_t>(param_idx, BigIntValue::Get(param));
			break;
		case LogicalTypeId::DOUBLE:
			Bind<double>(param_idx, DoubleValue::Get(param));
			break;
		case LogicalTypeId::BLOB:
			BindBlob(param_idx, StringValue::Get(param));
			break;
		case LogicalTypeId::VARCHAR:
			BindText(param_idx, StringValue::Get(param));
			break;
		default:
			throw InternalException("Unsupported parameter type \"%s\", index: %zu for SQLite::BindValue", param.type().ToString(), param_idx);
		}
	}
}

} // namespace duckdb
