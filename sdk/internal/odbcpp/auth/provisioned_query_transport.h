#pragma once
#include "odbcpp/auth/checked_response_boundary.h"
#include "odbcpp/auth/aws_db_xml_response.h"
namespace rs::core::auth {
// Fixed GetClusterCredentials/ClusterResult transport validation, NOT issuing proof.
class TransportQueryError final {
 public:
  explicit TransportQueryError(BoundaryFailure boundary) noexcept:boundary_(boundary) {}
  TransportQueryError(XmlError local,BoundaryFailure boundary) noexcept:xml_(local),boundary_(boundary) {}
  TransportQueryError(FieldError local,BoundaryFailure boundary) noexcept:field_(local),boundary_(boundary) {}
  const std::optional<XmlError>& xml() const noexcept { return xml_; }
  const std::optional<FieldError>& field() const noexcept { return field_; }
  BoundaryFailure boundary() const noexcept { return boundary_; }
 private:
  std::optional<XmlError> xml_;std::optional<FieldError> field_;BoundaryFailure boundary_;
};
using TransportQueryResult=std::variant<ExtractedDbFields,TransportQueryError>;
// Consumes body/lease before refusal; SAME owner original deadline; inactive body.
// Untrusted owning fields only. No scalar clock, UTC eligibility or authority API.
TransportQueryResult prevalidate_provisioned_query_transport(ResponseBytes&&,
    TransportBoundaryLease&&,std::string_view expected_user);
}
