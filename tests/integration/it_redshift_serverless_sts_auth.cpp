// Separately admitted serverless STS-assume-role -> GetCredentials -> TLS login.
// No ordinary CI run; no credentials or CA material in this source.
#include <gtest/gtest.h>
#include "core/database/postgres/pg_database_connection.h"
#include "odbcpp/transport/tls_transport.h"
#include "odbcpp/auth/aws/sts_assume_role_owner.h"
#include "odbcpp/auth/aws/serverless_credentials_owner.h"
#include <aws/core/config/AWSConfigFileProfileConfigLoader.h>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
namespace {
namespace a=rs::core::auth;
namespace sts=a::aws::sts;
namespace sl=a::aws::serverless;
namespace d=rs::core::database;
using Pg=d::postgres::PgDatabaseConnection;
constexpr std::string_view admission="serverless-sts-auth001";
const char* owner_failure_name(sts::OwnerFailure failure) noexcept {
  switch(failure){
    case sts::OwnerFailure::InvalidTarget:return "InvalidTarget";
    case sts::OwnerFailure::InvalidSource:return "InvalidSource";
    case sts::OwnerFailure::MainEntryRequired:return "MainEntryRequired";
    case sts::OwnerFailure::Unavailable:return "Unavailable";
    case sts::OwnerFailure::Consumed:return "Consumed";
    case sts::OwnerFailure::WrongThread:return "WrongThread";
    case sts::OwnerFailure::LocalFailure:return "LocalFailure";
    case sts::OwnerFailure::ProviderDenied:return "ProviderDenied";
  }
  return "Unknown";
}
const char* field_failure_name(a::FieldFailure failure) noexcept {
  switch(failure){
    case a::FieldFailure::UnsupportedOperation:return "UnsupportedOperation";
    case a::FieldFailure::InvalidShape:return "InvalidShape";
    case a::FieldFailure::ResourceLimit:return "ResourceLimit";
    case a::FieldFailure::InvalidFieldName:return "InvalidFieldName";
    case a::FieldFailure::UnknownField:return "UnknownField";
    case a::FieldFailure::MissingField:return "MissingField";
    case a::FieldFailure::DuplicateField:return "DuplicateField";
    case a::FieldFailure::WrongType:return "WrongType";
    case a::FieldFailure::InvalidUser:return "InvalidUser";
    case a::FieldFailure::InvalidPassword:return "InvalidPassword";
    case a::FieldFailure::InvalidTimestamp:return "InvalidTimestamp";
    case a::FieldFailure::Cancelled:return "Cancelled";
    case a::FieldFailure::AllocationFailed:return "AllocationFailed";
  }
  return "Unknown";
}
std::optional<std::string> selection(const char* key,std::size_t limit=256){
  const char* value=std::getenv(key);if(!value)return {};
  std::size_t size=0;while(size<=limit&&value[size]!='\0')++size;
  if(size==0||size>limit)return {};
  for(std::size_t i=0;i<size;++i){const auto c=static_cast<unsigned char>(value[i]);if(c<32||c==127)return {};}
  return std::string{value,size};
}
struct Spec {
  std::string region,account,role_name,role_session_name,workgroup,database,host,ca,profile,credentials,source,generation;
  unsigned duration_seconds;
};
std::optional<Spec> read_spec(){
  const auto region=selection("ODBCPP_SERVERLESS_STS_REGION");
  const auto account=selection("ODBCPP_SERVERLESS_STS_ACCOUNT");
  const auto role=selection("ODBCPP_SERVERLESS_STS_ROLE_NAME",512);
  const auto session=selection("ODBCPP_SERVERLESS_STS_ROLE_SESSION",256);
  const auto workgroup=selection("ODBCPP_SERVERLESS_STS_WORKGROUP");
  const auto database=selection("ODBCPP_SERVERLESS_STS_DATABASE");
  const auto host=selection("ODBCPP_SERVERLESS_STS_HOST");
  const auto ca=selection("ODBCPP_SERVERLESS_STS_CA_FILE",4096);
  const auto profile=selection("ODBCPP_SERVERLESS_STS_PROFILE");
  const auto credentials=selection("ODBCPP_SERVERLESS_STS_CREDENTIALS_FILE",4096);
  const auto source=selection("ODBCPP_SERVERLESS_STS_SOURCE_IDENTITY");
  const auto generation=selection("ODBCPP_SERVERLESS_STS_SOURCE_GENERATION");
  const auto duration=selection("ODBCPP_SERVERLESS_STS_DURATION_SECONDS");
  if(!region||!account||!role||!session||!workgroup||!database||!host||!ca||!profile||!credentials||!source||!generation||!duration)return {};
  if(ca->front()!='/'||credentials->front()!='/')return {};
  unsigned duration_seconds=0;
  for(char c:*duration){if(c<'0'||c>'9')return {};duration_seconds=duration_seconds*10+static_cast<unsigned>(c-'0');if(duration_seconds>3600)return {};}
  return Spec{*region,*account,*role,*session,*workgroup,*database,*host,*ca,*profile,*credentials,*source,*generation,duration_seconds};
}
struct SigningKeys { a::SecretBytes access,secret,token; };
std::optional<SigningKeys> load_static_keys(const Spec& spec){
  try{
    Aws::Config::AWSConfigFileProfileConfigLoader loader{spec.credentials.c_str(),false};
    if(!loader.Load())return {};
    const auto& profiles=loader.GetProfiles();
    const auto found=profiles.find(spec.profile.c_str());
    if(found==profiles.end())return {};
    const auto& value=found->second.GetCredentials();
    if(value.GetAWSAccessKeyId().empty()||value.GetAWSSecretKey().empty())return {};
    const auto secret=[](const Aws::String& bytes){return a::SecretBytes::create(
        std::as_bytes(std::span{bytes.data(),bytes.size()}));};
    auto access=secret(value.GetAWSAccessKeyId());auto key=secret(value.GetAWSSecretKey());
    auto token=secret(value.GetSessionToken());
    if(!access||!key||!token)return {};
    return SigningKeys{std::move(access).value(),std::move(key).value(),std::move(token).value()};
  }catch(...){return {};}
}
rs::util::Deadline deadline_60s(){
  const auto start=rs::util::Clock::now();
  const auto duration=std::chrono::duration_cast<rs::util::Clock::duration>(std::chrono::seconds{60});
  return start+duration;
}
// Assumes the role and issues serverless DB credentials. On success fills
// db_user (borrowed) and db_password (copied; test-local, wiped by SecretBytes
// owner). Returns false on any refusal without leaking source values.
struct IssuedCredentials { std::string user; a::SecretBytes password; };
std::optional<IssuedCredentials> issue_credentials(const Spec& spec,const std::string& role_name_override){
  auto keys=load_static_keys(spec);
  if(!keys)return {};
  auto cancel=std::make_shared<a::aws::provisioned_native::WorkerCancellation>();
  const auto deadline=deadline_60s();
  a::StsAssumeRoleTarget target{a::StsAssumeRolePartition::Aws,spec.account,spec.region,
      role_name_override,spec.role_session_name,spec.duration_seconds};
  auto assumed=sts::StsAssumeRoleOwner::create(std::move(target),std::move(keys->access),
      std::move(keys->secret),std::move(keys->token),deadline,cancel);
  if(!assumed.owner)return {};
  auto cred=assumed.owner->acquire();
  if(!cred)return {};
  if(!assumed.owner->close())return {};
  auto& c=cred.value();
  sl::ServerlessTarget st{spec.region,spec.workgroup,spec.database,spec.duration_seconds};
  auto sc=sl::ServerlessCredentialsOwner::create(std::move(st),std::move(c.access_key_id),
      std::move(c.secret_access_key),std::move(c.session_token),deadline_60s(),cancel);
  if(!sc.owner)return {};
  auto fields=sc.owner->acquire();
  if(!fields)return {};
  if(!sc.owner->close())return {};
  IssuedCredentials out{std::move(fields.value().user),std::move(fields.value().password)};
  return std::move(out);
}
TEST(ServerlessStsAuth, AssumeRoleGetCredentialsAndVerifiedTlsLogin){
  const char* marker=std::getenv("ODBCPP_SERVERLESS_STS_ADMISSION");
  if(!marker||std::string_view(marker)!=admission){
    GTEST_SKIP()<<"Separate serverless STS admission required";
  }
  auto spec=read_spec();ASSERT_TRUE(spec.has_value());
  auto issued=issue_credentials(*spec,spec->role_name);
  ASSERT_TRUE(issued.has_value());
  d::ConnectionSettings settings;
  settings.host=spec->host;settings.port=5439;settings.database=spec->database;
  settings.user=issued->user;settings.use_ssl=true;settings.ssl_ca_file=spec->ca;
  issued->password.with_bytes([&](auto bytes){settings.password.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());});
  auto transport=std::make_unique<rs::core::transport::TLSTransport>();
  transport->set_verify(true);transport->set_hostname_verification(true);transport->set_ca_locations(spec->ca,"");
  Pg connection{std::move(transport),std::nullopt,d::postgres::PgCatalogProfile::Redshift};
  auto connected=connection.connect_until(settings,deadline_60s());
  ASSERT_TRUE(connected.has_value());
  auto query=connection.execute_query("SELECT current_database(), TRIM(current_user), CAST(1 AS INTEGER), CAST(NULL AS INTEGER)",deadline_60s());
  ASSERT_TRUE(query.has_value());
  const auto& result=query.value();
  EXPECT_FALSE(result.error.has_value());
  ASSERT_EQ(result.rows.size(),1U);ASSERT_EQ(result.rows[0].size(),4U);
  EXPECT_TRUE(result.rows[0][0].has_value()&&*result.rows[0][0]==spec->database);
  EXPECT_TRUE(result.rows[0][2].has_value()&&*result.rows[0][2]=="1");
  EXPECT_FALSE(result.rows[0][3].has_value());
  connection.disconnect();
}
TEST(ServerlessStsAuth, ForeignRoleArnIsRefusedBeforeCredentialIssuance){
  const char* marker=std::getenv("ODBCPP_SERVERLESS_STS_ADMISSION");
  if(!marker||std::string_view(marker)!=admission){
    GTEST_SKIP()<<"Separate serverless STS admission required";
  }
  auto spec=read_spec();ASSERT_TRUE(spec.has_value());
  // A syntactically valid but non-existent role must refuse at assume-role.
  auto issued=issue_credentials(*spec,"definitely-not-a-real-role/absent");
  EXPECT_FALSE(issued.has_value());
}
TEST(ServerlessStsAuth, WrongWorkgroupIsRefusedAtGetCredentials){
  const char* marker=std::getenv("ODBCPP_SERVERLESS_STS_ADMISSION");
  if(!marker||std::string_view(marker)!=admission){
    GTEST_SKIP()<<"Separate serverless STS admission required";
  }
  auto spec=read_spec();ASSERT_TRUE(spec.has_value());
  auto keys=load_static_keys(*spec);ASSERT_TRUE(keys.has_value());
  auto cancel=std::make_shared<a::aws::provisioned_native::WorkerCancellation>();
  a::StsAssumeRoleTarget target{a::StsAssumeRolePartition::Aws,spec->account,spec->region,
      spec->role_name,spec->role_session_name,spec->duration_seconds};
  auto assumed=sts::StsAssumeRoleOwner::create(std::move(target),std::move(keys->access),
      std::move(keys->secret),std::move(keys->token),deadline_60s(),cancel);
  ASSERT_TRUE(assumed.owner);auto cred=assumed.owner->acquire();ASSERT_TRUE(cred);
  ASSERT_TRUE(assumed.owner->close());
  auto& c=cred.value();
  sl::ServerlessTarget st{spec->region,"definitely-not-a-workgroup",spec->database,spec->duration_seconds};
  auto sc=sl::ServerlessCredentialsOwner::create(std::move(st),std::move(c.access_key_id),
      std::move(c.secret_access_key),std::move(c.session_token),deadline_60s(),cancel);
  ASSERT_TRUE(sc.owner);auto fields=sc.owner->acquire();
  EXPECT_FALSE(fields);
  EXPECT_TRUE(sc.owner->close());
}
} // namespace
