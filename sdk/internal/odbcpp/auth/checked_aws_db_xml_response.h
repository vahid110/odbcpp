#pragma once
#include "odbcpp/auth/aws_db_xml_response.h"
#include "odbcpp/auth/checked_response_boundary.h"
namespace rs::core::auth {
class CheckedXmlError final {
 public:
  explicit CheckedXmlError(XmlError local) noexcept : local_(local) {}
  explicit CheckedXmlError(BoundaryFailure boundary) noexcept : boundary_(boundary) {}
  CheckedXmlError(XmlError local,BoundaryFailure boundary) noexcept : local_(local),boundary_(boundary) {}
  std::optional<XmlError> local() const noexcept { return local_; }
  std::optional<BoundaryFailure> boundary() const noexcept { return boundary_; }
  std::string_view safe_message() const noexcept {
    return boundary_?"Checked XML boundary refused":"Checked XML representation refused";
  }
 private:
  std::optional<XmlError> local_;
  std::optional<BoundaryFailure> boundary_;
};
template<class T> using CheckedXmlResult = std::variant<T,CheckedXmlError>;
// Post-c only, consumes inactive body and phase before representation refusal.
// Active with_bytes entry move is fatal borrower misuse, never healthy return.
// Snapshot is owning/untrusted; later extraction needs its own retained phase.
CheckedXmlResult<ResponseSnapshot> parse_aws_db_xml_response_checked(
    DbCredentialOperation,ResponseShape,ResponseBytes&&,ProcessingBoundaryLease&&);
namespace detail {
// Private closed allocation-refusal seam, not a provider/callback factory.
enum class CheckedXmlConstructionFailure { BeforeParse, BeforePublication };
class CheckedXmlTestAccess final {
 public:
  static CheckedXmlResult<ResponseSnapshot> refuse_processing(DbCredentialOperation,
      ResponseShape,ResponseBytes&&,ProcessingBoundaryLease&&,CheckedXmlConstructionFailure);
};
}
}
