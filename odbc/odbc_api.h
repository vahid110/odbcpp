#pragma once
#include "odbc_types.h"

// ODBC API function declarations
// ANSI and wide-character ODBC API declarations.
extern "C" {

// Handle management
SQLRETURN SQLAllocHandle(SQLSMALLINT handle_type, SQLHANDLE input_handle, SQLHANDLE* output_handle);
SQLRETURN SQLFreeHandle(SQLSMALLINT handle_type, SQLHANDLE handle);

// Connection management
SQLRETURN SQLConnect(SQLHDBC connection_handle, 
                    SQLCHAR* server_name, SQLSMALLINT name_length1,
                    SQLCHAR* user_name, SQLSMALLINT name_length2, 
                    SQLCHAR* authentication, SQLSMALLINT name_length3);
SQLRETURN SQLDriverConnect(
    SQLHDBC connection_handle, SQLHWND window_handle,
    SQLCHAR* connection_string_in, SQLSMALLINT string_length1,
    SQLCHAR* connection_string_out, SQLSMALLINT buffer_length,
    SQLSMALLINT* string_length2, SQLUSMALLINT driver_completion);
SQLRETURN SQLDisconnect(SQLHDBC connection_handle);
SQLRETURN SQLSetConnectAttr(SQLHDBC connection_handle, SQLINTEGER attribute,
                            SQLPOINTER value, SQLINTEGER string_length);
SQLRETURN SQLGetConnectAttr(SQLHDBC connection_handle, SQLINTEGER attribute,
                            SQLPOINTER value, SQLINTEGER buffer_length,
                            SQLINTEGER* string_length);
SQLRETURN SQLEndTran(SQLSMALLINT handle_type, SQLHANDLE handle,
                     SQLSMALLINT completion_type);

// Statement execution
SQLRETURN SQLExecDirect(SQLHSTMT statement_handle, SQLCHAR* statement_text, SQLINTEGER text_length);
SQLRETURN SQLFetch(SQLHSTMT statement_handle);
SQLRETURN SQLFetchScroll(SQLHSTMT statement_handle,
                         SQLSMALLINT fetch_orientation,
                         SQLLEN fetch_offset);
SQLRETURN SQLMoreResults(SQLHSTMT statement_handle);
SQLRETURN SQLGetData(SQLHSTMT statement_handle, SQLUSMALLINT column_number, SQLSMALLINT target_type,
                    void* target_value, SQLLEN buffer_length, SQLLEN* strlen_or_indicator);
SQLRETURN SQLRowCount(SQLHSTMT statement_handle, SQLLEN* row_count);
SQLRETURN SQLGetTypeInfo(SQLHSTMT statement_handle, SQLSMALLINT data_type);
SQLRETURN SQLColumns(
    SQLHSTMT statement_handle, SQLCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLCHAR* schema_name,
    SQLSMALLINT name_length2, SQLCHAR* table_name,
    SQLSMALLINT name_length3, SQLCHAR* column_name,
    SQLSMALLINT name_length4);
SQLRETURN SQLColumnsW(
    SQLHSTMT statement_handle, SQLWCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLWCHAR* schema_name,
    SQLSMALLINT name_length2, SQLWCHAR* table_name,
    SQLSMALLINT name_length3, SQLWCHAR* column_name,
    SQLSMALLINT name_length4);
SQLRETURN SQLPrimaryKeys(
    SQLHSTMT statement_handle, SQLCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLCHAR* schema_name,
    SQLSMALLINT name_length2, SQLCHAR* table_name,
    SQLSMALLINT name_length3);
SQLRETURN SQLPrimaryKeysW(
    SQLHSTMT statement_handle, SQLWCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLWCHAR* schema_name,
    SQLSMALLINT name_length2, SQLWCHAR* table_name,
    SQLSMALLINT name_length3);
SQLRETURN SQLForeignKeys(
    SQLHSTMT statement_handle, SQLCHAR* pk_catalog_name,
    SQLSMALLINT name_length1, SQLCHAR* pk_schema_name,
    SQLSMALLINT name_length2, SQLCHAR* pk_table_name,
    SQLSMALLINT name_length3, SQLCHAR* fk_catalog_name,
    SQLSMALLINT name_length4, SQLCHAR* fk_schema_name,
    SQLSMALLINT name_length5, SQLCHAR* fk_table_name,
    SQLSMALLINT name_length6);
SQLRETURN SQLForeignKeysW(
    SQLHSTMT statement_handle, SQLWCHAR* pk_catalog_name,
    SQLSMALLINT name_length1, SQLWCHAR* pk_schema_name,
    SQLSMALLINT name_length2, SQLWCHAR* pk_table_name,
    SQLSMALLINT name_length3, SQLWCHAR* fk_catalog_name,
    SQLSMALLINT name_length4, SQLWCHAR* fk_schema_name,
    SQLSMALLINT name_length5, SQLWCHAR* fk_table_name,
    SQLSMALLINT name_length6);
SQLRETURN SQLStatistics(
    SQLHSTMT statement_handle, SQLCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLCHAR* schema_name,
    SQLSMALLINT name_length2, SQLCHAR* table_name,
    SQLSMALLINT name_length3, SQLUSMALLINT unique,
    SQLUSMALLINT reserved);
SQLRETURN SQLStatisticsW(
    SQLHSTMT statement_handle, SQLWCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLWCHAR* schema_name,
    SQLSMALLINT name_length2, SQLWCHAR* table_name,
    SQLSMALLINT name_length3, SQLUSMALLINT unique,
    SQLUSMALLINT reserved);
SQLRETURN SQLProcedures(
    SQLHSTMT statement_handle, SQLCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLCHAR* schema_name,
    SQLSMALLINT name_length2, SQLCHAR* procedure_name,
    SQLSMALLINT name_length3);
SQLRETURN SQLProceduresW(
    SQLHSTMT statement_handle, SQLWCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLWCHAR* schema_name,
    SQLSMALLINT name_length2, SQLWCHAR* procedure_name,
    SQLSMALLINT name_length3);
SQLRETURN SQLProcedureColumns(
    SQLHSTMT statement_handle, SQLCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLCHAR* schema_name,
    SQLSMALLINT name_length2, SQLCHAR* procedure_name,
    SQLSMALLINT name_length3, SQLCHAR* column_name,
    SQLSMALLINT name_length4);
SQLRETURN SQLProcedureColumnsW(
    SQLHSTMT statement_handle, SQLWCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLWCHAR* schema_name,
    SQLSMALLINT name_length2, SQLWCHAR* procedure_name,
    SQLSMALLINT name_length3, SQLWCHAR* column_name,
    SQLSMALLINT name_length4);
SQLRETURN SQLSpecialColumns(
    SQLHSTMT statement_handle, SQLUSMALLINT identifier_type,
    SQLCHAR* catalog_name, SQLSMALLINT name_length1,
    SQLCHAR* schema_name, SQLSMALLINT name_length2,
    SQLCHAR* table_name, SQLSMALLINT name_length3,
    SQLUSMALLINT scope, SQLUSMALLINT nullable);
SQLRETURN SQLSpecialColumnsW(
    SQLHSTMT statement_handle, SQLUSMALLINT identifier_type,
    SQLWCHAR* catalog_name, SQLSMALLINT name_length1,
    SQLWCHAR* schema_name, SQLSMALLINT name_length2,
    SQLWCHAR* table_name, SQLSMALLINT name_length3,
    SQLUSMALLINT scope, SQLUSMALLINT nullable);
SQLRETURN SQLTables(
    SQLHSTMT statement_handle, SQLCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLCHAR* schema_name,
    SQLSMALLINT name_length2, SQLCHAR* table_name,
    SQLSMALLINT name_length3, SQLCHAR* table_type,
    SQLSMALLINT name_length4);
SQLRETURN SQLTablesW(
    SQLHSTMT statement_handle, SQLWCHAR* catalog_name,
    SQLSMALLINT name_length1, SQLWCHAR* schema_name,
    SQLSMALLINT name_length2, SQLWCHAR* table_name,
    SQLSMALLINT name_length3, SQLWCHAR* table_type,
    SQLSMALLINT name_length4);
SQLRETURN SQLSetStmtAttr(SQLHSTMT statement_handle, SQLINTEGER attribute,
                         SQLPOINTER value, SQLINTEGER string_length);
SQLRETURN SQLGetStmtAttr(SQLHSTMT statement_handle, SQLINTEGER attribute,
                         SQLPOINTER value, SQLINTEGER buffer_length,
                         SQLINTEGER* string_length);
SQLRETURN SQLCloseCursor(SQLHSTMT statement_handle);
SQLRETURN SQLFreeStmt(SQLHSTMT statement_handle, SQLUSMALLINT option);

// Error handling
SQLRETURN SQLGetDiagRec(SQLSMALLINT handle_type, SQLHANDLE handle, SQLSMALLINT rec_number,
                       SQLCHAR* sqlstate, SQLINTEGER* native_error, SQLCHAR* message_text,
                       SQLSMALLINT buffer_length, SQLSMALLINT* text_length);

SQLRETURN SQLGetDiagField(SQLSMALLINT handle_type, SQLHANDLE handle, SQLSMALLINT rec_number,
                         SQLSMALLINT diag_identifier, SQLPOINTER diag_info_ptr, SQLSMALLINT buffer_length,
                         SQLSMALLINT* string_length_ptr);
SQLRETURN SQLGetDiagFieldW(
    SQLSMALLINT handle_type, SQLHANDLE handle, SQLSMALLINT rec_number,
    SQLSMALLINT diag_identifier, SQLPOINTER diag_info_ptr,
    SQLSMALLINT buffer_length, SQLSMALLINT* string_length_ptr);

SQLRETURN SQLError(SQLHENV environment_handle, SQLHDBC connection_handle, SQLHSTMT statement_handle,
                  SQLCHAR* sqlstate, SQLINTEGER* native_error, SQLCHAR* message_text,
                  SQLSMALLINT buffer_length, SQLSMALLINT* text_length);
SQLRETURN SQLErrorW(
    SQLHENV environment_handle, SQLHDBC connection_handle,
    SQLHSTMT statement_handle, SQLWCHAR* sqlstate,
    SQLINTEGER* native_error, SQLWCHAR* message_text,
    SQLSMALLINT buffer_length, SQLSMALLINT* text_length);

// Driver information
SQLRETURN SQLGetInfo(SQLHDBC connection_handle, SQLUSMALLINT info_type, 
                    void* info_value, SQLSMALLINT buffer_length, SQLSMALLINT* string_length);
SQLRETURN SQLGetInfoW(SQLHDBC connection_handle, SQLUSMALLINT info_type,
                      void* info_value, SQLSMALLINT buffer_length,
                      SQLSMALLINT* string_length);
SQLRETURN SQLGetFunctions(SQLHDBC connection_handle, SQLUSMALLINT function_id,
                          SQLUSMALLINT* supported);
SQLRETURN SQLNativeSql(
    SQLHDBC connection_handle, SQLCHAR* input_statement,
    SQLINTEGER text_length1, SQLCHAR* output_statement,
    SQLINTEGER buffer_length, SQLINTEGER* text_length2);

// Environment attributes
SQLRETURN SQLSetEnvAttr(SQLHENV environment_handle, SQLINTEGER attribute, 
                       void* value, SQLINTEGER string_length);
SQLRETURN SQLGetEnvAttr(SQLHENV environment_handle, SQLINTEGER attribute,
                        SQLPOINTER value, SQLINTEGER buffer_length,
                        SQLINTEGER* string_length);

// Prepared statements
SQLRETURN SQLPrepare(SQLHSTMT statement_handle, SQLCHAR* statement_text, SQLINTEGER text_length);
SQLRETURN SQLExecute(SQLHSTMT statement_handle);
SQLRETURN SQLNumParams(SQLHSTMT statement_handle, SQLSMALLINT* parameter_count);
SQLRETURN SQLBindParameter(SQLHSTMT statement_handle, SQLUSMALLINT parameter_number, SQLSMALLINT input_output_type,
                          SQLSMALLINT value_type, SQLSMALLINT parameter_type, SQLULEN column_size,
                          SQLSMALLINT decimal_digits, SQLPOINTER parameter_value, SQLLEN buffer_length,
                          SQLLEN* strlen_or_indicator);

// Column binding
SQLRETURN SQLBindCol(SQLHSTMT statement_handle, SQLUSMALLINT column_number, SQLSMALLINT target_type,
                    SQLPOINTER target_value, SQLLEN buffer_length, SQLLEN* strlen_or_indicator);

// Result set metadata
SQLRETURN SQLNumResultCols(SQLHSTMT statement_handle, SQLSMALLINT* column_count);
SQLRETURN SQLDescribeCol(SQLHSTMT statement_handle, SQLUSMALLINT column_number,
                        SQLCHAR* column_name, SQLSMALLINT name_buffer_length, SQLSMALLINT* name_length,
                        SQLSMALLINT* data_type, SQLULEN* column_size, SQLSMALLINT* decimal_digits,
                        SQLSMALLINT* nullable);
SQLRETURN SQLDescribeColW(
    SQLHSTMT statement_handle, SQLUSMALLINT column_number,
    SQLWCHAR* column_name, SQLSMALLINT name_buffer_length,
    SQLSMALLINT* name_length, SQLSMALLINT* data_type,
    SQLULEN* column_size, SQLSMALLINT* decimal_digits,
    SQLSMALLINT* nullable);
SQLRETURN SQLColAttribute(SQLHSTMT statement_handle, SQLUSMALLINT column_number, SQLUSMALLINT field_identifier,
                         SQLPOINTER character_attribute, SQLSMALLINT buffer_length, SQLSMALLINT* string_length,
                         SQLLEN* numeric_attribute);
SQLRETURN SQLColAttributeW(
    SQLHSTMT statement_handle, SQLUSMALLINT column_number,
    SQLUSMALLINT field_identifier, SQLPOINTER character_attribute,
    SQLSMALLINT buffer_length, SQLSMALLINT* string_length,
    SQLLEN* numeric_attribute);

// Parameter metadata
SQLRETURN SQLDescribeParam(SQLHSTMT statement_handle, SQLUSMALLINT parameter_number, SQLSMALLINT* data_type,
                          SQLULEN* parameter_size, SQLSMALLINT* decimal_digits, SQLSMALLINT* nullable);

// SQLGetDescField and SQLSetDescField are declared in the system ODBC headers.

SQLRETURN SQLConnectW(SQLHDBC connection_handle, 
                     SQLWCHAR* server_name, SQLSMALLINT name_length1,
                     SQLWCHAR* user_name, SQLSMALLINT name_length2, 
                     SQLWCHAR* authentication, SQLSMALLINT name_length3);
SQLRETURN SQLDriverConnectW(
    SQLHDBC connection_handle, SQLHWND window_handle,
    SQLWCHAR* connection_string_in, SQLSMALLINT string_length1,
    SQLWCHAR* connection_string_out, SQLSMALLINT buffer_length,
    SQLSMALLINT* string_length2, SQLUSMALLINT driver_completion);
SQLRETURN SQLExecDirectW(SQLHSTMT statement_handle,
                         SQLWCHAR* statement_text, SQLINTEGER text_length);
SQLRETURN SQLPrepareW(SQLHSTMT statement_handle,
                      SQLWCHAR* statement_text, SQLINTEGER text_length);
SQLRETURN SQLGetDiagRecW(SQLSMALLINT handle_type, SQLHANDLE handle, SQLSMALLINT rec_number,
                        SQLWCHAR* sqlstate, SQLINTEGER* native_error, SQLWCHAR* message_text,
                        SQLSMALLINT buffer_length, SQLSMALLINT* text_length);
SQLRETURN SQLNativeSqlW(
    SQLHDBC connection_handle, SQLWCHAR* input_statement,
    SQLINTEGER text_length1, SQLWCHAR* output_statement,
    SQLINTEGER buffer_length, SQLINTEGER* text_length2);

} // extern "C"
