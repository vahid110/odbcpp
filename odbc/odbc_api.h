#pragma once
#include "odbc_types.h"

// ODBC API function declarations
// Architecture supports both ANSI and Wide versions
// Currently implementing ANSI only, Wide versions reserved for future
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
SQLRETURN SQLGetData(SQLHSTMT statement_handle, SQLUSMALLINT column_number, SQLSMALLINT target_type,
                    void* target_value, SQLLEN buffer_length, SQLLEN* strlen_or_indicator);
SQLRETURN SQLRowCount(SQLHSTMT statement_handle, SQLLEN* row_count);
SQLRETURN SQLGetTypeInfo(SQLHSTMT statement_handle, SQLSMALLINT data_type);
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

SQLRETURN SQLError(SQLHENV environment_handle, SQLHDBC connection_handle, SQLHSTMT statement_handle,
                  SQLCHAR* sqlstate, SQLINTEGER* native_error, SQLCHAR* message_text,
                  SQLSMALLINT buffer_length, SQLSMALLINT* text_length);

// Driver information
SQLRETURN SQLGetInfo(SQLHDBC connection_handle, SQLUSMALLINT info_type, 
                    void* info_value, SQLSMALLINT buffer_length, SQLSMALLINT* string_length);
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
SQLRETURN SQLColAttribute(SQLHSTMT statement_handle, SQLUSMALLINT column_number, SQLUSMALLINT field_identifier,
                         SQLPOINTER character_attribute, SQLSMALLINT buffer_length, SQLSMALLINT* string_length,
                         SQLLEN* numeric_attribute);

// Parameter metadata
SQLRETURN SQLDescribeParam(SQLHSTMT statement_handle, SQLUSMALLINT parameter_number, SQLSMALLINT* data_type,
                          SQLULEN* parameter_size, SQLSMALLINT* decimal_digits, SQLSMALLINT* nullable);

// Note: SQLGetDescField and SQLSetDescField are declared in system sql.h

// Wide character versions (reserved for future implementation)
// These ensure our architecture can support Unicode without breaking changes
#ifdef ODBCPP_ENABLE_WIDE_FUNCTIONS
typedef wchar_t SQLWCHAR;

// Wide function declarations (not implemented yet)
SQLRETURN SQLConnectW(SQLHDBC connection_handle, 
                     SQLWCHAR* server_name, SQLSMALLINT name_length1,
                     SQLWCHAR* user_name, SQLSMALLINT name_length2, 
                     SQLWCHAR* authentication, SQLSMALLINT name_length3);
SQLRETURN SQLExecDirectW(SQLHSTMT statement_handle, SQLWCHAR* statement_text, SQLINTEGER text_length);
SQLRETURN SQLGetDiagRecW(SQLSMALLINT handle_type, SQLHANDLE handle, SQLSMALLINT rec_number,
                        SQLWCHAR* sqlstate, SQLINTEGER* native_error, SQLWCHAR* message_text,
                        SQLSMALLINT buffer_length, SQLSMALLINT* text_length);
#endif // ODBCPP_ENABLE_WIDE_FUNCTIONS

} // extern "C"
