#include "odbcpp/auth/aws/provisioned_pg_connector.h"
#include "odbcpp/auth/pg_credential_consumer.h"
#include "core/database/postgres/pg_database_connection.h"
namespace rs::core::auth::aws::provisioned_native {
ConnectorOutcome connect_provisioned_observation_until(
    rs::core::database::postgres::PgDatabaseConnection& session,
    const rs::core::database::ConnectionSettings& selected, Observation& observation) {
  ConnectorOutcome result;
  const auto* request=observation.bound_request();
  if(!request) {
    result.failure=ConnectorFailure::InvalidObservation;
    result.counts=observation.counts();
    return result;
  }
  bool connected_here=false;
  try {
    result.borrow_passed=observation.with_fields([&](const ExtractedDbFields& fields) {
      result.entered=true;
      const auto& binding=request->binding();
      result.wire_identity=fields.user==binding.target().principal;
      result.password_present=!fields.password.empty() && fields.password.size()<=SecretBytes::max_bytes;
      result.raw_expiry_positive=fields.expiry.microseconds_since_epoch>0;
      if(binding.target().service!=Service::Redshift ||
         !result.wire_identity || !result.password_present || !result.raw_expiry_positive) {
        result.failure=ConnectorFailure::ConnectionRejected;
        return;
      }
      result.connection.emplace(connect_bound_temporary_db_until(session,selected,*request,fields));
      connected_here=result.connection->has_value();
      if(!connected_here)result.failure=ConnectorFailure::ConnectionRejected;
    });
    if(!result.borrow_passed && !result.failure)
      result.failure=ConnectorFailure::ObservationRejected;
  } catch(...) {
    result.failure=ConnectorFailure::LocalFailure; // Never expose exception context.
  }
  result.observation_closed=observation.close();
  result.counts=observation.counts();
  if(!result.observation_closed || result.counts.boundary || result.counts.n_samples!=1) {
    if(!result.failure)result.failure=ConnectorFailure::ObservationRejected;
  }
  if(result.failure && connected_here)session.disconnect();
  return result;
}
} // namespace rs::core::auth::aws::provisioned_native
