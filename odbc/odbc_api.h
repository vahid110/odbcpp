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
SQLRETURN SQLDisconnect(SQLHDBC connection_handle);

// Statement execution
SQLRETURN SQLExecDirect(SQLHSTMT statement_handle, SQLCHAR* statement_text, SQLINTEGER text_length);
SQLRETURN SQLFetch(SQLHSTMT statement_handle);
SQLRETURN SQLGetData(SQLHSTMT statement_handle, SQLUSMALLINT column_number, SQLSMALLINT target_type,
                    void* target_value, SQLLEN buffer_length, SQLLEN* strlen_or_indicator);

// Error handling
SQLRETURN SQLGetDiagRec(SQLSMALLINT handle_type, SQLHANDLE handle, SQLSMALLINT rec_number,
                       SQLCHAR* sqlstate, SQLINTEGER* native_error, SQLCHAR* message_text,
                       SQLSMALLINT buffer_length, SQLSMALLINT* text_length);

// Driver information
SQLRETURN SQLGetInfo(SQLHDBC connection_handle, SQLUSMALLINT info_type, 
                    void* info_value, SQLSMALLINT buffer_length, SQLSMALLINT* string_length);

// Environment attributes
SQLRETURN SQLSetEnvAttr(SQLHENV environment_handle, SQLINTEGER attribute, 
                       void* value, SQLINTEGER string_length);

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