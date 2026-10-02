#pragma once

namespace rs::core::database::postgres {

// Private PostgreSQL-wire-family policy, selected explicitly by composition.
// It is not inferred from server names, connection options or protocol packets.
enum class PgCatalogProfile { PostgreSQL, Redshift };

}  // namespace rs::core::database::postgres
