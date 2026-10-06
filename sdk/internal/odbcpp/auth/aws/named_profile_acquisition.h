#pragma once
#include "odbcpp/auth/aws/provisioned_native_owner.h"
#include <functional>
namespace rs::core::auth::aws::provisioned_native {
// Public selection only: no credentials, authority flag, provider or live override.
struct ProtectedNamedSourceSpec {
  std::string profile, credentials_path, config_path, source_identity, generation;
};
namespace detail {
// Pure offset sequencing, not descriptor ownership or source authority. Only the
// private stage supplies its validated owned descriptors to actual SDK paths.
enum class NamedReadPhase { CacheInitialization, ExplicitProvider };
class NamedFdReadCursor final {
public:
 NamedFdReadCursor(int credentials,int config);
 ~NamedFdReadCursor();
 NamedFdReadCursor(const NamedFdReadCursor&)=delete;
 NamedFdReadCursor& operator=(const NamedFdReadCursor&)=delete;
 const std::string& credentials_path() const noexcept { return credentials_path_; }
 const std::string& config_path() const noexcept { return config_path_; }
 bool rewind(NamedReadPhase,const std::function<bool(int)>&) noexcept;
 bool failed() const noexcept { return failed_; }
private:
 int credentials_,config_;std::string credentials_path_,config_path_;
 bool cache_{},provider_{},busy_{},failed_{};
};
class ProtectedNamedStage final {
 public:
  static std::unique_ptr<ProtectedNamedStage> create(const ProtectedNamedSourceSpec&,const Context&,const Request&);
  // Darwin-only fd aliases share offsets. Descriptors remain owned through SDK
  // shutdown. This owner creates/removes no secret files or directories.
  ~ProtectedNamedStage();
  ProtectedNamedStage(const ProtectedNamedStage&)=delete;
  ProtectedNamedStage& operator=(const ProtectedNamedStage&)=delete;
  bool unchanged() const noexcept;
  bool rewind_for_cache() noexcept;
  bool rewind_for_provider() noexcept;
  const std::string& credentials_path() const noexcept;
  const std::string& config_path() const noexcept;
  const std::string& directory() const noexcept;
  const std::string& profile() const noexcept;
 private:
  struct Storage;
  explicit ProtectedNamedStage(std::unique_ptr<Storage>);
  std::unique_ptr<Storage> storage_;
};
class NamedSourceAcquisitionOwner final {
 public:
  static bool load(State&) noexcept;
  static FrozenNamedSource pending(const Context&);
};
}
} // namespace
