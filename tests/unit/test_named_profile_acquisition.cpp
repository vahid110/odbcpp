#include <gtest/gtest.h>
#include "odbcpp/auth/aws/named_profile_acquisition.h"
#include <aws/core/Globals.h>
#include <chrono>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
using namespace std::chrono_literals;
namespace a=rs::core::auth;
namespace p=a::aws::provisioned_native;
namespace {
const std::string valid="[pilot]\naws_access_key_id=synthetic-access-key\naws_secret_access_key=synthetic-secret-key\naws_session_token=synthetic-token\n";
struct Files {
 std::string dir,credential,config;
 Files(){char pattern[]="/private/tmp/odbcpp_named_test_XXXXXX";auto* made=::mkdtemp(pattern);if(!made)std::terminate();dir=made;credential=dir+"/credentials";config=dir+"/config";write(credential,valid);write(config,"");}
 static void write(const std::string& path,std::string_view body){std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(body.data(),static_cast<std::streamsize>(body.size()));file.close();if(!file||::chmod(path.c_str(),0600))std::terminate();}
 ~Files(){::unlink((dir+"/other").c_str());::unlink((dir+"/saved").c_str());::unlink(credential.c_str());::unlink(config.c_str());::chmod(dir.c_str(),0700);::rmdir(dir.c_str());}
 p::ProtectedNamedSourceSpec spec()const{return {"pilot",credential,config,"synthetic-source","g1"};}
};
class Reader final:public a::ResponseObservationSource {
 public:rs::util::Deadline now{10s};unsigned reads{};std::function<void()> callback;
 a::ResponseClockRead read_monotonic()override{++reads;if(callback)callback();return now;}
 bool cancellation_requested()override{return false;}
};
struct NamedSetup {
 std::shared_ptr<Reader> reader=std::make_shared<Reader>();std::shared_ptr<p::WorkerCancellation> cancel=std::make_shared<p::WorkerCancellation>();
 std::shared_ptr<a::ResponseSourceGeneration> generation;
 NamedSetup(){auto request=p::FixedFixture::request(p::FixedFixture::context());auto out=a::ResponseSourceGeneration::create(request.binding());generation=std::get<std::shared_ptr<a::ResponseSourceGeneration>>(std::move(out));}
 p::CreateOutcome make(p::ProtectedNamedSourceSpec spec){return p::FixedFixture::create_named(std::move(spec),reader,generation,cancel);}
};
std::unique_ptr<p::detail::ProtectedNamedStage> stage(const Files& f){auto c=p::FixedFixture::context();return p::detail::ProtectedNamedStage::create(f.spec(),c,p::FixedFixture::request(c));}
}
TEST(NamedProfileAcquisition, SameSdkScopeLoadsExactlyOnceAndFreezesSource){
 Files files;NamedSetup setup;auto made=setup.make(files.spec());ASSERT_TRUE(made.owner);EXPECT_EQ(made.counts.initializations,1U);EXPECT_EQ(made.counts.named_provider_loads,1U);EXPECT_TRUE(made.counts.named_source_ready);
 auto out=made.owner->acquire_observation();ASSERT_TRUE(out.observation);EXPECT_TRUE(out.counts.model_success);EXPECT_EQ(out.counts.selected_sends,1U);EXPECT_GT(out.counts.frozen_provider_reads,0U);EXPECT_EQ(out.counts.named_provider_loads,1U);EXPECT_EQ(out.counts.shutdowns,1U);EXPECT_FALSE(out.counts.named_stage_released);
 EXPECT_TRUE(out.observation->close());EXPECT_TRUE(made.owner->close());EXPECT_TRUE(made.owner->counts().named_stage_released);EXPECT_EQ(made.owner->counts().initializations,1U);
 EXPECT_EQ(::access(files.credential.c_str(),F_OK),0); // original source never removed
}
TEST(NamedProfileAcquisition, StrictPolicyRefusesBeforeInitialization){
 for(const auto& body:std::initializer_list<std::string>{"[wrong]\naws_access_key_id=x\naws_secret_access_key=y\n",valid+"aws_access_key_id=again\n",valid+"[other]\n",valid+"role_arn=unsupported\n",valid+"credential_process=unsupported\n",valid+"sso_session=unsupported\n","[pilot]\naws_access_key_id=\naws_secret_access_key=y\n"}){
  Files files;Files::write(files.credential,body);NamedSetup setup;auto made=setup.make(files.spec());EXPECT_FALSE(made.owner);EXPECT_TRUE(made.failure);EXPECT_EQ(made.counts.initializations,0U);EXPECT_EQ(made.counts.selected_sends,0U);
 }
 Files files;Files::write(files.config,"[default]\n");EXPECT_ANY_THROW(stage(files));
}
TEST(NamedProfileAcquisition, PrivateFdValidationRefusesUnsafeSources){
 for(unsigned kind=0;kind<6;++kind){SCOPED_TRACE(kind);Files files;
  if(kind==0)ASSERT_EQ(::chmod(files.dir.c_str(),0755),0);
  if(kind==1)ASSERT_EQ(::chmod(files.credential.c_str(),0644),0);
  if(kind==2){ASSERT_EQ(::rename(files.credential.c_str(),(files.dir+"/saved").c_str()),0);ASSERT_EQ(::symlink("saved",files.credential.c_str()),0);}
  if(kind==3)ASSERT_EQ(::link(files.credential.c_str(),(files.dir+"/other").c_str()),0);
  if(kind==4)ASSERT_EQ(::unlink(files.credential.c_str()),0);
  if(kind==5)Files::write(files.credential,std::string(16385,'x'));
  EXPECT_ANY_THROW(stage(files));
 }
}
TEST(NamedProfileAcquisition, StageSnapshotRefusesChangedOriginalOrReopenedPath){
 for(unsigned kind=0;kind<3;++kind){SCOPED_TRACE(kind);Files files;auto owned=stage(files);ASSERT_TRUE(owned->unchanged());
  if(kind==0)Files::write(files.credential,valid+"\n");
  if(kind==1){ASSERT_EQ(::rename(files.credential.c_str(),(files.dir+"/saved").c_str()),0);Files::write(files.credential,valid);}
  if(kind==2)Files::write(files.config,"changed");
  EXPECT_FALSE(owned->unchanged());
 }
}
TEST(NamedProfileAcquisition, ExactSourceGenerationAndProfileNeverFallback){
 Files files;
 for(unsigned kind=0;kind<4;++kind){auto spec=files.spec();if(kind==0)spec.source_identity="foreign";if(kind==1)spec.generation="g2";if(kind==2)spec.profile="default";if(kind==3)spec.config_path=spec.credentials_path;NamedSetup setup;auto made=setup.make(std::move(spec));EXPECT_FALSE(made.owner);EXPECT_TRUE(made.failure);EXPECT_EQ(made.counts.named_provider_loads,0U);EXPECT_EQ(made.counts.selected_sends,0U);}
}
TEST(NamedProfileAcquisition, OriginalDeadlineCancellationAndRetirementRefusePublication){
 for(unsigned kind=0;kind<7;++kind){SCOPED_TRACE(kind);Files files;NamedSetup setup;
  if(kind==0)setup.reader->now=rs::util::Deadline{20s};
  if(kind==1)setup.cancel->cancel();
  if(kind==2)(void)setup.generation->retire(a::SourceRetirement::Closed);
  if(kind==3)setup.reader->callback=[&]{if(Aws::GetDefaultClientBootstrap())setup.reader->now=rs::util::Deadline{20s};};
  // Read12 is the retained post-load checkpoint. The fd route adds two
  // original-D checks after offset rewinds, before Init and the provider read.
  if(kind>=4)setup.reader->callback=[&]{if(setup.reader->reads==12){if(kind==4)setup.reader->now=rs::util::Deadline{20s};if(kind==5)setup.cancel->cancel();if(kind==6)(void)setup.generation->retire(a::SourceRetirement::Closed);}};
  auto made=setup.make(files.spec());EXPECT_FALSE(made.owner);EXPECT_TRUE(made.failure);EXPECT_EQ(made.counts.selected_sends,0U);
  if(kind>=4){EXPECT_EQ(made.counts.named_provider_loads,1U);EXPECT_FALSE(made.counts.named_source_ready);EXPECT_EQ(made.counts.shutdowns,1U);EXPECT_TRUE(made.counts.named_stage_released);}
  else{EXPECT_EQ(made.counts.named_provider_loads,0U);}
 }
}
TEST(NamedProfileAcquisition, OutstandingSecretBorrowPreventsOwnerReleaseAndSingleConsume){
 Files files;NamedSetup setup;auto made=setup.make(files.spec());ASSERT_TRUE(made.owner);auto out=made.owner->acquire_observation();ASSERT_TRUE(out.observation);
 EXPECT_FALSE(made.owner->close());bool seen=false;EXPECT_TRUE(out.observation->with_fields([&](const a::ExtractedDbFields& fields){seen=fields.user=="IAM:fixture_user";EXPECT_FALSE(made.owner->close());}));EXPECT_TRUE(seen);
 auto again=made.owner->acquire_observation();EXPECT_FALSE(again.observation);EXPECT_EQ(again.failure,p::Failure::Consumed);
 auto moved=std::move(*out.observation);EXPECT_FALSE(out.observation->with_fields([](const auto&){}));EXPECT_TRUE(moved.close());EXPECT_TRUE(made.owner->close());EXPECT_EQ(made.owner->counts().shutdowns,1U);
}
TEST(NamedProfileAcquisition, StageOwnershipCleanupDoesNotDeleteSourceAndNoRawErrors){
 Files files;{auto owned=stage(files);EXPECT_TRUE(owned->directory().empty());EXPECT_TRUE(owned->unchanged());
  EXPECT_EQ(owned->credentials_path().find("/dev/fd/"),0U);EXPECT_EQ(owned->config_path().find("/dev/fd/"),0U);
  EXPECT_NE(owned->credentials_path(),files.credential);EXPECT_NE(owned->config_path(),files.config);}
 EXPECT_EQ(::access(files.credential.c_str(),F_OK),0);EXPECT_EQ(::access(files.config.c_str(),F_OK),0);
 NamedSetup setup;setup.reader->callback=[&]{if(setup.reader->reads==2)setup.reader->now=rs::util::Deadline{9s};};auto made=setup.make(files.spec());EXPECT_FALSE(made.owner);EXPECT_TRUE(made.failure);EXPECT_EQ(made.counts.selected_sends,0U);
}

// Pure mocks below perform no fd/file/provider/SDK operation. Synthetic numbers
// exercise the offset plan only and cannot enter a ProtectedNamedStage owner.
TEST(NamedProfileFdCursor, CacheThenProviderRewindsBothSharedOffsetsOnce) {
 p::detail::NamedFdReadCursor cursor{31,47};
 EXPECT_EQ(cursor.credentials_path(),"/dev/fd/31");EXPECT_EQ(cursor.config_path(),"/dev/fd/47");
 std::vector<int> order;const auto reset=[&](int fd){order.push_back(fd);return true;};
 EXPECT_TRUE(cursor.rewind(p::detail::NamedReadPhase::CacheInitialization,reset));
 EXPECT_TRUE(cursor.rewind(p::detail::NamedReadPhase::ExplicitProvider,reset));
 EXPECT_EQ(order,(std::vector<int>{31,47,31,47}));
 EXPECT_FALSE(cursor.rewind(p::detail::NamedReadPhase::ExplicitProvider,reset));
 EXPECT_EQ(order.size(),4U);EXPECT_TRUE(cursor.failed());
}
TEST(NamedProfileFdCursor, ProviderBeforeCacheAndInvalidDescriptorsNeverReset) {
 unsigned calls=0;const auto reset=[&](int){++calls;return true;};
 for(const auto& pair:std::initializer_list<std::pair<int,int>>{{-1,4},{0,4},{2,4},{4,4}}){
  p::detail::NamedFdReadCursor cursor{pair.first,pair.second};EXPECT_TRUE(cursor.failed());
  EXPECT_TRUE(cursor.credentials_path().empty());EXPECT_TRUE(cursor.config_path().empty());
  EXPECT_FALSE(cursor.rewind(p::detail::NamedReadPhase::CacheInitialization,reset));
 }
 p::detail::NamedFdReadCursor cursor{8,9};
 EXPECT_FALSE(cursor.rewind(p::detail::NamedReadPhase::ExplicitProvider,reset));
 EXPECT_FALSE(cursor.rewind(p::detail::NamedReadPhase::CacheInitialization,reset));EXPECT_EQ(calls,0U);
}
TEST(NamedProfileFdCursor, FailedPartialResetCannotRetryOrPublishAProviderPhase) {
 for(unsigned fail_at:{1U,2U}){
  p::detail::NamedFdReadCursor cursor{8,9};unsigned calls=0;
  const auto reset=[&](int){return ++calls!=fail_at;};
  EXPECT_FALSE(cursor.rewind(p::detail::NamedReadPhase::CacheInitialization,reset));
  EXPECT_EQ(calls,fail_at);EXPECT_TRUE(cursor.failed());
  const auto healed=[&](int){++calls;return true;};
  EXPECT_FALSE(cursor.rewind(p::detail::NamedReadPhase::CacheInitialization,healed));
  EXPECT_FALSE(cursor.rewind(p::detail::NamedReadPhase::ExplicitProvider,healed));EXPECT_EQ(calls,fail_at);
 }
}
TEST(NamedProfileFdCursor, ExceptionAndReentryLatchBeforeAnotherDescriptorReset) {
 {p::detail::NamedFdReadCursor cursor{8,9};unsigned calls=0;
  EXPECT_FALSE(cursor.rewind(p::detail::NamedReadPhase::CacheInitialization,[&](int)->bool{++calls;throw 7;}));
  EXPECT_TRUE(cursor.failed());EXPECT_EQ(calls,1U);
  EXPECT_FALSE(cursor.rewind(p::detail::NamedReadPhase::ExplicitProvider,[&](int){++calls;return true;}));EXPECT_EQ(calls,1U);}
 {p::detail::NamedFdReadCursor cursor{8,9};unsigned calls=0;
  EXPECT_FALSE(cursor.rewind(p::detail::NamedReadPhase::CacheInitialization,[&](int){++calls;
   EXPECT_FALSE(cursor.rewind(p::detail::NamedReadPhase::ExplicitProvider,[](int){return true;}));return true;}));
  EXPECT_TRUE(cursor.failed());EXPECT_EQ(calls,1U);}
}
TEST(NamedProfileFdCursor, RepeatedCacheOrUnknownPhaseIsPermanentWithoutMoreReads) {
 for(unsigned kind:{0U,1U}){p::detail::NamedFdReadCursor cursor{8,9};unsigned calls=0;
  const auto reset=[&](int){++calls;return true;};
  ASSERT_TRUE(cursor.rewind(p::detail::NamedReadPhase::CacheInitialization,reset));
  const auto phase=kind==0?p::detail::NamedReadPhase::CacheInitialization:static_cast<p::detail::NamedReadPhase>(99);
  EXPECT_FALSE(cursor.rewind(phase,reset));EXPECT_FALSE(cursor.rewind(p::detail::NamedReadPhase::ExplicitProvider,reset));
  EXPECT_TRUE(cursor.failed());EXPECT_EQ(calls,2U);}
}
