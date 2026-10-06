#pragma once
#include "odbcpp/auth/auth_core.h"
#include "odbcpp/auth/aws_db_response_fields.h"
#include "odbcpp/database/i_database_connection.h"
namespace rs::core::database::postgres { class PgDatabaseConnection; }
namespace rs::core::auth {
// Private PostgreSQL-family wire consumer, separate from provider-free AuthCore.
// Invoke only inside the issuer's active fields borrow. This validates binding
// consistency, not issuer provenance or acquisition expiry. The embedding must
// disconnect if its enclosing observation subsequently fails. Successful session
// ownership stays with the caller; the supplied Pg transport must verify peers.
rs::core::database::BackendResult<void> connect_bound_temporary_db_until(
    rs::core::database::postgres::PgDatabaseConnection&,
    const rs::core::database::ConnectionSettings&, const Request&,
    const ExtractedDbFields&);
} // namespace rs::core::auth
