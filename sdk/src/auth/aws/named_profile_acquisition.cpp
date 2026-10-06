#include "odbcpp/auth/aws/named_profile_acquisition.h"
#include "odbcpp/auth/aws/provisioned_native_http_ingress.h"
#include "odbcpp/auth/aws/explicit_profile_source.h"
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
namespace rs::core::auth::aws::provisioned_native::detail {
NamedFdReadCursor::NamedFdReadCursor(int credentials,int config):credentials_(credentials),config_(config) {
 if(credentials<3||config<3||credentials==config){failed_=true;return;}
 credentials_path_="/dev/fd/"+std::to_string(credentials);
 config_path_="/dev/fd/"+std::to_string(config);
}
NamedFdReadCursor::~NamedFdReadCursor(){if(busy_)std::terminate();}
bool NamedFdReadCursor::rewind(NamedReadPhase phase,const std::function<bool(int)>& reset) noexcept {
 if(failed_)return false;
 if(busy_){failed_=true;return false;}
 if(!reset||(phase!=NamedReadPhase::CacheInitialization&&phase!=NamedReadPhase::ExplicitProvider)||
    (phase==NamedReadPhase::CacheInitialization?cache_:(!cache_||provider_))){failed_=true;return false;}
 busy_=true;
 // Consume before callbacks. A failed or reentrant reset cannot retry/heal.
 if(phase==NamedReadPhase::CacheInitialization)cache_=true;else provider_=true;
 try {
  if(!reset(credentials_)||failed_||!reset(config_)||failed_)failed_=true;
 }catch(...){failed_=true;}
 busy_=false;return !failed_;
}
namespace {
struct Refused {};
struct Fd {int value{-1};~Fd(){if(value>=0)::close(value);}Fd()=default;Fd(const Fd&)=delete;Fd& operator=(const Fd&)=delete;};
bool same(const struct stat& a,const struct stat& b) noexcept {
 return a.st_dev==b.st_dev && a.st_ino==b.st_ino && a.st_size==b.st_size && a.st_uid==b.st_uid && a.st_mode==b.st_mode && a.st_nlink==b.st_nlink &&
  a.st_mtimespec.tv_sec==b.st_mtimespec.tv_sec && a.st_mtimespec.tv_nsec==b.st_mtimespec.tv_nsec && a.st_ctimespec.tv_sec==b.st_ctimespec.tv_sec && a.st_ctimespec.tv_nsec==b.st_ctimespec.tv_nsec;
}
bool atom(std::string_view v) noexcept {if(v.empty()||v.size()>256||v=="."||v=="..")return false;for(unsigned char c:v)if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.'))return false;return true;}
int private_parent(const std::string& path,std::string& parent,std::string& leaf){
 if(path.size()>4096||path.empty()||path.front()!='/')throw Refused{};
 const auto last=path.rfind('/');parent=path.substr(0,last);leaf=path.substr(last+1);if(parent.empty()||!atom(leaf))throw Refused{};
 Fd current;current.value=::open("/",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(current.value<0)throw Refused{};
 std::string_view rest{parent};rest.remove_prefix(1);
 while(!rest.empty()){auto end=rest.find('/');auto part=rest.substr(0,end);if(!atom(part))throw Refused{};std::string component{part};int next=::openat(current.value,component.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(next<0)throw Refused{};::close(current.value);current.value=next;if(end==rest.npos)break;rest.remove_prefix(end+1);}
 struct stat info{},named{};if(::fstat(current.value,&info)||::lstat(parent.c_str(),&named)||!S_ISDIR(info.st_mode)||info.st_uid!=::geteuid()||(info.st_mode&07777)!=0700||info.st_dev!=named.st_dev||info.st_ino!=named.st_ino)throw Refused{};
 return std::exchange(current.value,-1);
}
void source_file(int parent,const std::string& leaf,Fd& fd,struct stat& stamp,std::size_t limit){
 fd.value=::openat(parent,leaf.c_str(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
 if(fd.value>=0&&fd.value<3){const int duplicate=::fcntl(fd.value,F_DUPFD_CLOEXEC,3);if(duplicate<0)throw Refused{};::close(fd.value);fd.value=duplicate;}
 if(fd.value<0||::fstat(fd.value,&stamp)||!S_ISREG(stamp.st_mode)||stamp.st_uid!=::geteuid()||(stamp.st_mode&07777)!=0600||stamp.st_nlink!=1||stamp.st_size<0||static_cast<std::uint64_t>(stamp.st_size)>limit)throw Refused{};
}
SecretBytes read_secret(int fd,std::size_t limit){
 std::array<std::byte,16385> bytes{};struct Wipe{decltype(bytes)& value;~Wipe(){volatile std::byte* p=value.data();for(std::size_t i=0;i<value.size();++i)p[i]=std::byte{};}}wipe{bytes};
 std::size_t used=0;while(used<=limit){auto n=::pread(fd,bytes.data()+used,limit+1-used,static_cast<off_t>(used));if(n<0){if(errno==EINTR)continue;throw Refused{};}if(n==0)break;used+=static_cast<std::size_t>(n);}
 if(used>limit)throw Refused{};auto result=SecretBytes::create({bytes.data(),used});if(!result)throw Refused{};return std::move(result).value();
}
}
struct ProtectedNamedStage::Storage {
 ProtectedNamedSourceSpec spec;std::string original_parent,credential_leaf,config_leaf,dir;
 Fd parent,original_credentials,original_config;
 struct stat parent_stamp{},credential_stamp{},config_stamp{};
 std::optional<NamedFdReadCursor> cursor;
 const std::thread::id creator{std::this_thread::get_id()};bool failed{};
 ~Storage(){if(creator!=std::this_thread::get_id())std::terminate();}
};
ProtectedNamedStage::ProtectedNamedStage(std::unique_ptr<Storage> s):storage_(std::move(s)){}
ProtectedNamedStage::~ProtectedNamedStage()=default;
std::unique_ptr<ProtectedNamedStage> ProtectedNamedStage::create(const ProtectedNamedSourceSpec& spec,const Context& context,const Request& request){
#if !defined(__APPLE__)
 (void)spec;(void)context;(void)request;throw Refused{};
#endif
 // /dev/fd duplication semantics are selected only for Darwin. No path fallback.
 if(!atom(spec.profile)||spec.source_identity!=context.source_identity||spec.generation!=context.source_generation||request.binding().source().identity!=spec.source_identity||request.binding().source().generation!=spec.generation||spec.credentials_path==spec.config_path)throw Refused{};
 auto s=std::make_unique<Storage>();s->spec=spec;s->parent.value=private_parent(spec.credentials_path,s->original_parent,s->credential_leaf);
 std::string other_parent;Fd other;other.value=private_parent(spec.config_path,other_parent,s->config_leaf);if(other_parent!=s->original_parent)throw Refused{};
 if(::fstat(s->parent.value,&s->parent_stamp))throw Refused{};
 source_file(s->parent.value,s->credential_leaf,s->original_credentials,s->credential_stamp,16384);source_file(s->parent.value,s->config_leaf,s->original_config,s->config_stamp,0);
 auto bytes=read_secret(s->original_credentials.value,16384);(void)read_secret(s->original_config.value,0);
 // Reuse only the existing closed grammar validator. Refusing stubs are never invoked;
 // no injected projection/read value is promoted to filesystem or issuer proof.
 auto generation=std::make_shared<profile::SourceGeneration>(spec.generation);
 auto validation=profile::ExplicitProfileSource::create({spec.profile,spec.credentials_path,spec.config_path,spec.source_identity,spec.generation,"opened-file"},{profile::StagingProjection::Kind::Regular,0700,0600,1,false,"opened-file"},std::move(bytes),request,generation,
  []()->std::variant<rs::util::Deadline,profile::ReadFailure>{return profile::ReadFailure::Failed;},
  [](const auto&,const auto&)->std::variant<profile::CredentialParts,profile::LoadFailure>{return profile::LoadFailure::Failed;});
 if(!std::holds_alternative<std::unique_ptr<profile::ExplicitProfileSource>>(validation))throw Refused{};
 s->cursor.emplace(s->original_credentials.value,s->original_config.value);
 if(s->cursor->failed())throw Refused{};
 auto out=std::unique_ptr<ProtectedNamedStage>{new ProtectedNamedStage{std::move(s)}};if(!out->unchanged())throw Refused{};return out;
}
bool ProtectedNamedStage::unchanged()const noexcept {
 const auto& s=*storage_;if(s.creator!=std::this_thread::get_id()||s.failed||!s.cursor||s.cursor->failed())return false;struct stat now{};
 const auto check=[&](int fd,int parent,const char* leaf,const struct stat& stamp){struct stat own{},named{};return !::fstat(fd,&own)&&same(own,stamp)&&!::fstatat(parent,leaf,&named,AT_SYMLINK_NOFOLLOW)&&same(named,stamp);};
 if(::lstat(s.original_parent.c_str(),&now)||now.st_dev!=s.parent_stamp.st_dev||now.st_ino!=s.parent_stamp.st_ino||now.st_uid!=::geteuid()||(now.st_mode&07777)!=0700)return false;
 return check(s.original_credentials.value,s.parent.value,s.credential_leaf.c_str(),s.credential_stamp)&&check(s.original_config.value,s.parent.value,s.config_leaf.c_str(),s.config_stamp);
}
bool ProtectedNamedStage::rewind_for_cache() noexcept {
 if(storage_->creator!=std::this_thread::get_id())return false;
 if(!unchanged()){storage_->failed=true;return false;}
 const auto reset=[](int fd){off_t result;do{result=::lseek(fd,0,SEEK_SET);}while(result<0&&errno==EINTR);return result==0;};
 if(!storage_->cursor->rewind(NamedReadPhase::CacheInitialization,reset)||!unchanged()){storage_->failed=true;return false;}
 return true;
}
bool ProtectedNamedStage::rewind_for_provider() noexcept {
 if(storage_->creator!=std::this_thread::get_id())return false;
 if(!unchanged()){storage_->failed=true;return false;}
 const auto reset=[](int fd){off_t result;do{result=::lseek(fd,0,SEEK_SET);}while(result<0&&errno==EINTR);return result==0;};
 if(!storage_->cursor->rewind(NamedReadPhase::ExplicitProvider,reset)||!unchanged()){storage_->failed=true;return false;}
 return true;
}
const std::string& ProtectedNamedStage::credentials_path()const noexcept{return storage_->cursor->credentials_path();}
const std::string& ProtectedNamedStage::config_path()const noexcept{return storage_->cursor->config_path();}
const std::string& ProtectedNamedStage::directory()const noexcept{return storage_->dir;}
const std::string& ProtectedNamedStage::profile()const noexcept{return storage_->spec.profile;}
FrozenNamedSource NamedSourceAcquisitionOwner::pending(const Context& c){FrozenNamedSource out{Aws::Auth::AWSCredentials{},c.source_identity,c.source_generation};out.value_.reset();out.owns_=false;return out;}
bool NamedSourceAcquisitionOwner::load(State& state) noexcept {
 try{
  if(!state.initialized||state.source_ready||!state.protected_stage||!state.check()||!state.protected_stage->rewind_for_provider()||!state.check()){state.reject();return false;}
  Aws::Auth::AWSCredentials value;
  {Aws::Auth::ProfileConfigFileAWSCredentialsProvider provider{state.protected_stage->profile().c_str()};++state.metrics.named_provider_loads;
   value=provider.Aws::Auth::ProfileConfigFileAWSCredentialsProvider::GetAWSCredentials();}
  if(!state.check()||!state.protected_stage->unchanged()||value.GetAWSAccessKeyId().empty()||value.GetAWSSecretKey().empty()){state.reject();return false;}
  state.source.value_.emplace(std::move(value));state.source.owns_=true;state.source_ready=true;state.metrics.named_source_ready=true;return true;
 }catch(...){state.reject();return false;}
}
} // namespace
