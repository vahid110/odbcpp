// Separately admitted JwtIamAuthPlugin flow: JWT -> STS AssumeRoleWithWebIdentity
// -> redshift-serverless GetCredentials -> verified TLS login.
// No ordinary CI run; no token, credentials or CA material in this source.
#include <gtest/gtest.h>
#include "core/database/postgres/pg_database_connection.h"
#include "odbcpp/transport/tls_transport.h"
#include "odbcpp/auth/aws/jwt_iam_auth.h"
#include "odbcpp/auth/aws/serverless_credentials_owner.h"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
namespace {
namespace a=rs::core::auth;
namespace jwt=a::aws::jwt_iam;
namespace sl=a::aws::serverless;
namespace d=rs::core::database;
using Pg=d::postgres::PgDatabaseConnection;
constexpr std::string_view admission="serverless-jwt-iam-auth001";
std::optional<std::string> selection(const char* key,std::size_t limit=256){
  const char* value=std::getenv(key);if(!value)return {};
  std::size_t size=0;while(size<=limit&&value[size]!='\0')++size;
  if(size==0||size>limit)return {};
  for(std::size_t i=0;i<size;++i){const auto c=static_cast<unsigned char>(value[i]);if(c<32||c==127)return {};}
  return std::string{value,size};
}
struct Spec {
  std::string token_file,role_arn,region,workgroup,database,host,ca;
  unsigned duration_seconds;
};
std::optional<Spec> read_spec(){
  const auto token_file=selection("ODBCPP_JWT_IAM_TOKEN_FILE",4096);
  const auto role_arn=selection("ODBCPP_JWT_IAM_ROLE_ARN",2048);
  const auto region=selection("ODBCPP_JWT_IAM_REGION");
  const auto workgroup=selection("ODBCPP_JWT_IAM_WORKGROUP");
  const auto database=selection("ODBCPP_JWT_IAM_DATABASE");
  const auto host=selection("ODBCPP_JWT_IAM_HOST");
  const auto ca=selection("ODBCPP_JWT_IAM_CA_FILE",4096);
  const auto duration=selection("ODBCPP_JWT_IAM_DURATION_SECONDS");
  if(!token_file||!role_arn||!region||!workgroup||!database||!host||!ca||!duration)return {};
  if(token_file->front()!='/'||ca->front()!='/')return {};
  unsigned duration_seconds=0;
  for(char c:*duration){if(c<'0'||c>'9')return {};duration_seconds=duration_seconds*10+static_cast<unsigned>(c-'0');if(duration_seconds>3600)return {};}
  return Spec{*token_file,*role_arn,*region,*workgroup,*database,*host,*ca,duration_seconds};
}
// The token file must be a private regular file owned by this user.
std::optional<a::SecretBytes> read_token(const std::string& path){
  struct stat st{};
  if(::stat(path.c_str(),&st)!=0||!S_ISREG(st.st_mode)||st.st_uid!=::getuid()||(st.st_mode&077)!=0)return {};
  std::ifstream in(path,std::ios::binary);if(!in)return {};
  std::string text((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
  while(!text.empty()&&(text.back()=='\n'||text.back()=='\r'||text.back()==' '))text.pop_back();
  if(text.empty())return {};
  auto made=a::SecretBytes::create(std::as_bytes(std::span{text.data(),text.size()}));
  std::fill(text.begin(),text.end(),'\0');
  if(!made)return {};
  return std::move(made).value();
}
rs::util::Deadline deadline_60s(){
  return rs::util::Clock::now()+std::chrono::duration_cast<rs::util::Clock::duration>(std::chrono::seconds{60});
}
struct IssuedCredentials { std::string user; a::SecretBytes password; };
std::optional<IssuedCredentials> issue_credentials(const Spec& spec,a::SecretBytes token){
  auto cancel=std::make_shared<a::aws::provisioned_native::WorkerCancellation>();
  auto assumed=jwt::assume_role_with_jwt(spec.role_arn,std::move(token),spec.region,"",spec.duration_seconds,
      deadline_60s(),cancel);
  if(!assumed)return {};
  auto& c=assumed.value();
  auto sc=sl::ServerlessCredentialsOwner::create({spec.region,spec.workgroup,spec.database,spec.duration_seconds},
      std::move(c.access_key_id),std::move(c.secret_access_key),std::move(c.session_token),deadline_60s(),cancel);
  if(!sc.owner)return {};
  auto fields=sc.owner->acquire();
  if(!fields)return {};
  if(!sc.owner->close())return {};
  return IssuedCredentials{std::move(fields.value().user),std::move(fields.value().password)};
}
bool admitted(){
  const char* marker=std::getenv("ODBCPP_JWT_IAM_ADMISSION");
  return marker&&std::string_view(marker)==admission;
}
TEST(ServerlessJwtIamAuth, WebIdentityGetCredentialsAndVerifiedTlsLogin){
  if(!admitted()){
    GTEST_SKIP()<<"Separate JwtIamAuthPlugin admission required";
  }
  auto spec=read_spec();ASSERT_TRUE(spec.has_value());
  auto token=read_token(spec->token_file);ASSERT_TRUE(token.has_value());
  EXPECT_TRUE(jwt::jwt_db_user(*token).has_value());
  auto issued=issue_credentials(*spec,std::move(*token));
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
  auto query=connection.execute_query("SELECT current_database(), TRIM(current_user), CAST(1 AS INTEGER)",deadline_60s());
  ASSERT_TRUE(query.has_value());
  const auto& result=query.value();
  EXPECT_FALSE(result.error.has_value());
  ASSERT_EQ(result.rows.size(),1U);ASSERT_EQ(result.rows[0].size(),3U);
  EXPECT_TRUE(result.rows[0][0].has_value()&&*result.rows[0][0]==spec->database);
  ASSERT_TRUE(result.rows[0][1].has_value());
  EXPECT_TRUE(*result.rows[0][1] == issued->user)
      << "Authenticated database principal differs from the issued user";
  EXPECT_TRUE(result.rows[0][2].has_value()&&*result.rows[0][2]=="1");
  connection.disconnect();
}
TEST(ServerlessJwtIamAuth, TamperedTokenIsRefusedBySts){
  if(!admitted()){
    GTEST_SKIP()<<"Separate JwtIamAuthPlugin admission required";
  }
  auto spec=read_spec();ASSERT_TRUE(spec.has_value());
  auto token=read_token(spec->token_file);ASSERT_TRUE(token.has_value());
  // Flip the first signature character (six data bits; the last base64url
  // character can carry only padding bits): STS must reject the signature.
  std::string text;
  token->with_bytes([&](auto bytes){text.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());});
  const auto signature=text.rfind('.');
  ASSERT_NE(std::string::npos,signature);
  ASSERT_LT(signature+1,text.size());
  char& first=text[signature+1];
  first=first=='A'?'B':'A';
  auto tampered=a::SecretBytes::create(std::as_bytes(std::span{text.data(),text.size()}));
  std::fill(text.begin(),text.end(),'\0');
  ASSERT_TRUE(tampered);
  auto refused = jwt::assume_role_with_jwt(spec->role_arn, std::move(tampered).value(),
      spec->region, "", spec->duration_seconds, deadline_60s(),
      std::make_shared<a::aws::provisioned_native::WorkerCancellation>());
  ASSERT_FALSE(refused);
  EXPECT_EQ(a::StsAssumeRoleFailure::ServiceRejected, refused.error().failure);
}
TEST(ServerlessJwtIamAuth, UntrustedRoleIsRefusedBySts){
  if(!admitted()){
    GTEST_SKIP()<<"Separate JwtIamAuthPlugin admission required";
  }
  auto spec=read_spec();ASSERT_TRUE(spec.has_value());
  auto token=read_token(spec->token_file);ASSERT_TRUE(token.has_value());
  auto foreign=*spec;
  const auto slash=foreign.role_arn.rfind('/');
  ASSERT_NE(std::string::npos,slash);
  foreign.role_arn=foreign.role_arn.substr(0,slash+1)+"definitely-not-a-real-role";
  auto refused = jwt::assume_role_with_jwt(foreign.role_arn, std::move(*token), foreign.region,
      "", foreign.duration_seconds, deadline_60s(),
      std::make_shared<a::aws::provisioned_native::WorkerCancellation>());
  ASSERT_FALSE(refused);
  EXPECT_EQ(a::StsAssumeRoleFailure::ServiceRejected, refused.error().failure);
}
} // namespace
