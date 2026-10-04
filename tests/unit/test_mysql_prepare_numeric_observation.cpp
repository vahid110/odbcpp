#include <gtest/gtest.h>
#include "tests/integration/mysql_prepare_numeric_observation.h"

namespace {
using Observer=rs::core::database::mysql::integration_detail::PrepareNumericObservation;
using Progress=Observer::Progress;
using Bytes=std::vector<std::byte>;
void integer(Bytes& b,std::uint64_t n,unsigned width) {
  for (unsigned i=0;i<width;++i) b.push_back(static_cast<std::byte>((n>>(8*i))&255));
}
Bytes frame(Bytes payload,unsigned sequence) {
  Bytes out;integer(out,payload.size(),3);integer(out,sequence,1);out.insert(out.end(),payload.begin(),payload.end());return out;
}
Bytes prepare(unsigned parameters=1,unsigned results=0) {
  Bytes b{std::byte{0}};integer(b,17,4);integer(b,results,2);integer(b,parameters,2);integer(b,0,1);integer(b,0,2);return b;
}
Bytes column(unsigned type=12,unsigned width=104,unsigned decimals=6,unsigned charset=45,std::string_view name="p") {
  Bytes b;
  for (auto field:{std::string_view("def"),std::string_view{},std::string_view{},std::string_view{},name,std::string_view{}}) {
    integer(b,field.size(),1);for (auto ch:field) b.push_back(static_cast<std::byte>(ch));
  }
  integer(b,12,1);integer(b,charset,2);integer(b,width,4);integer(b,type,1);integer(b,0,2);integer(b,decimals,1);integer(b,0,2);return b;
}
Bytes eof() { return {std::byte{254},std::byte{0},std::byte{0},std::byte{2},std::byte{0}}; }
void start(Observer& o,unsigned parameters=1,unsigned results=0) {
  o.begin();ASSERT_EQ(Progress::Collecting,o.observe_complete_frame(frame(prepare(parameters,results),1)));
}
TEST(MySqlPrepareNumericObservationTest, ExplicitBeginAndResetNeverInferOwnershipFromUnrelatedOk) {
  Observer o;EXPECT_EQ(Progress::Inconclusive,o.observe_complete_frame(frame(prepare(),1)));EXPECT_EQ(0u,o.record_count());
  start(o);ASSERT_EQ(Progress::Collecting,o.observe_complete_frame(frame(column(),2)));EXPECT_EQ(1u,o.record_count());
  o.missing();EXPECT_EQ(Progress::Inconclusive,o.progress());EXPECT_EQ(1u,o.record_count());
  EXPECT_EQ(Progress::Inconclusive,o.observe_complete_frame(frame(eof(),3)));
  o.begin();EXPECT_EQ(0u,o.record_count());EXPECT_FALSE(o.records_truncated());EXPECT_EQ(0u,o.parameter_count());
  EXPECT_EQ(Progress::Complete,o.observe_complete_frame(frame(prepare(0,0),1)));EXPECT_EQ(0u,o.record_count());
}
TEST(MySqlPrepareNumericObservationTest, RawSyntheticExpandedMetadataOwnsNumbersBeforeNormalizationRefusal) {
  for (unsigned q=0;q<=6;++q) for (unsigned charset:{63u,45u}) {
    Observer o;start(o,1,1);const auto semantic=19u+(q?1+q:0);const auto width=semantic*(charset==45?4:1);
    auto payload=column(12,width,q,charset,"private-name");
    ASSERT_EQ(Progress::Collecting,o.observe_complete_frame(frame(payload,2)));
    const auto owned=o.records()[0];payload.assign(payload.size(),std::byte{0});
    EXPECT_EQ(charset,owned.charset);EXPECT_EQ(12u,owned.type);EXPECT_EQ(width,owned.width);EXPECT_EQ(q,owned.decimals);
    EXPECT_EQ(Progress::Collecting,o.observe_complete_frame(frame(eof(),3)));
    EXPECT_EQ(Progress::Collecting,o.observe_complete_frame(frame(column(10,10,0,63,"result"),4)));
    EXPECT_EQ(1u,o.record_count());EXPECT_EQ(Progress::Complete,o.observe_complete_frame(frame(eof(),5)));
    // Default result normalization is unchanged. Structural capture supplies
    // neither NativeTypeInfo nor any admission for these synthetic widths.
    auto raw=rs::core::database::mysql::query_detail::NativeParameterDescriptorObservation{8,777,33,444};
    auto normalized=rs::core::database::mysql::query_detail::column(column(12,width,q,charset),rs::core::database::ResultLimits{},nullptr,nullptr,nullptr,&raw);
    if (charset==45) {
      EXPECT_FALSE(normalized);EXPECT_EQ(8u,raw.type);EXPECT_EQ(777u,raw.width);EXPECT_EQ(444u,raw.charset);
    } else {
      ASSERT_TRUE(normalized);EXPECT_EQ(semantic,normalized->normalized_type->column_size);EXPECT_EQ(q,normalized->normalized_type->decimal_digits);
    }
  }
}
TEST(MySqlPrepareNumericObservationTest, UnknownCharsetTypeAndNullValuesCannotManufactureNormalizedValidity) {
  for (unsigned charset:{0u,8u,46u,255u,65535u}) {
    Observer o;start(o);ASSERT_EQ(Progress::Collecting,o.observe_complete_frame(frame(column(7,0xffffffffu,255,charset),2)));
    EXPECT_EQ(charset,o.records()[0].charset);EXPECT_EQ(7u,o.records()[0].type);EXPECT_EQ(0xffffffffu,o.records()[0].width);
    EXPECT_EQ(255u,o.records()[0].decimals);EXPECT_EQ(Progress::Complete,o.observe_complete_frame(frame(eof(),3)));
    EXPECT_FALSE(rs::core::database::mysql::query_detail::column(column(7,0xffffffffu,255,charset),rs::core::database::ResultLimits{}));
  }
  // Caller NULL is not an input to this helper. A NULL row marker is never a
  // descriptor, while a real MYSQL_TYPE_NULL expression remains raw type6.
  Observer bad;start(bad);EXPECT_EQ(Progress::Malformed,bad.observe_complete_frame(frame({std::byte{251}},2)));EXPECT_EQ(0u,bad.record_count());
  Observer expression;start(expression);EXPECT_EQ(Progress::Collecting,expression.observe_complete_frame(frame(column(6,0,0,63),2)));EXPECT_EQ(6u,expression.records()[0].type);
}
TEST(MySqlPrepareNumericObservationTest, ActualCountsSeparateParameterAndResultColumnsAndReportTruncation) {
  Observer o;start(o,4,2);EXPECT_TRUE(o.records_truncated());EXPECT_EQ(4u,o.parameter_count());EXPECT_EQ(2u,o.result_count());
  for (unsigned i=0;i<4;++i) { EXPECT_EQ(Progress::Collecting,o.observe_complete_frame(frame(column(12,100+i,6,45),2+i))); }
  EXPECT_EQ(3u,o.record_count());EXPECT_EQ(102u,o.records()[2].width);
  EXPECT_EQ(Progress::Collecting,o.observe_complete_frame(frame(eof(),6)));
  EXPECT_EQ(Progress::Collecting,o.observe_complete_frame(frame(column(10,10,0,63),7)));
  EXPECT_EQ(Progress::Collecting,o.observe_complete_frame(frame(column(8,19,0,63),8)));
  EXPECT_EQ(Progress::Complete,o.observe_complete_frame(frame(eof(),9)));EXPECT_EQ(3u,o.record_count());
  Observer results;start(results,0,1);EXPECT_EQ(Progress::Collecting,results.observe_complete_frame(frame(column(),2)));
  EXPECT_EQ(Progress::Complete,results.observe_complete_frame(frame(eof(),3)));EXPECT_EQ(0u,results.record_count());
}
TEST(MySqlPrepareNumericObservationTest, EveryPayloadPrefixAndMalformedTailPublishesNoPartialRecord) {
  const auto payload=column();
  for (std::size_t n=0;n<payload.size();++n) {
    Observer o;start(o);EXPECT_EQ(Progress::Malformed,o.observe_complete_frame(frame(Bytes(payload.begin(),payload.begin()+n),2)))<<n;
    EXPECT_EQ(0u,o.record_count());EXPECT_EQ(0u,o.records()[0].width);
  }
  for (unsigned fault=0;fault<6;++fault) {
    auto malformed=payload;
    if (fault==0) { malformed.push_back(std::byte{0}); }
    if (fault==1) { malformed.back()=std::byte{1}; }
    if (fault==2) { malformed[10]=std::byte{11}; }
    if (fault==3) { malformed[1]=std::byte{'x'}; }
    if (fault==4) { malformed[8]=std::byte{0xff}; }
    if (fault==5) { malformed[8]=std::byte{0}; }
    Observer o;start(o);EXPECT_EQ(Progress::Malformed,o.observe_complete_frame(frame(malformed,2)));EXPECT_EQ(0u,o.record_count());
  }
}
TEST(MySqlPrepareNumericObservationTest, FragmentOrMissingFrameIsTerminalInconclusiveWithExplicitPartialEvidence) {
  const auto complete=frame(column(),3);
  for (std::size_t n=0;n<complete.size();++n) {
    Observer o;start(o,2);ASSERT_EQ(Progress::Collecting,o.observe_complete_frame(frame(column(10,40,0,45),2)));
    EXPECT_EQ(Progress::Inconclusive,o.observe_complete_frame(std::span(complete).first(n)));
    EXPECT_EQ(1u,o.record_count());EXPECT_EQ(10u,o.records()[0].type);
    EXPECT_EQ(Progress::Inconclusive,o.observe_complete_frame(complete));
  }
  Observer missing;start(missing);missing.missing();EXPECT_EQ(Progress::Inconclusive,missing.progress());EXPECT_EQ(0u,missing.record_count());
}
TEST(MySqlPrepareNumericObservationTest, SequenceAndDeclaredColumnEofBoundariesCannotBeBypassed) {
  for (unsigned fault=0;fault<5;++fault) {
    Observer o;start(o,1,1);
    if (fault==0) { EXPECT_EQ(Progress::Malformed,o.observe_complete_frame(frame(column(),3))); }
    else if (fault==1) { EXPECT_EQ(Progress::Malformed,o.observe_complete_frame(frame(eof(),2))); }
    else {
      ASSERT_EQ(Progress::Collecting,o.observe_complete_frame(frame(column(),2)));
      if (fault==2) { EXPECT_EQ(Progress::Malformed,o.observe_complete_frame(frame(column(),3))); }
      else {
        ASSERT_EQ(Progress::Collecting,o.observe_complete_frame(frame(eof(),3)));
        if (fault==3) { EXPECT_EQ(Progress::Malformed,o.observe_complete_frame(frame(eof(),4))); }
        else {
          ASSERT_EQ(Progress::Collecting,o.observe_complete_frame(frame(column(),4)));
          EXPECT_EQ(Progress::Malformed,o.observe_complete_frame(frame({std::byte{254}},5)));
        }
      }
    }
    EXPECT_EQ(Progress::Malformed,o.progress());EXPECT_NE(Progress::Complete,o.progress());
  }
}
TEST(MySqlPrepareNumericObservationTest, FiniteBudgetsRejectCountsFramesNamesAndWireWithoutMoreRecords) {
  for (unsigned fault=0;fault<7;++fault) {
    Observer::Limits limits;
    if (fault==0) { limits.max_metadata_entries=5; }
    if (fault==1) { limits.max_frames=2; }
    if (fault==2) { limits.max_wire_bytes=15; }
    if (fault==3) { limits.max_metadata_name_bytes=3; }
    if (fault==4) { limits.max_column_name_bytes=0; }
    if (fault==5) { limits.max_wire_bytes=16; }
    Observer o(limits);o.begin();auto first=o.observe_complete_frame(frame(prepare(fault==6?65535:1,0),1));
    if (fault<3 || fault==6) { EXPECT_EQ(Progress::LimitExceeded,first); }
    else { ASSERT_EQ(Progress::Collecting,first);EXPECT_EQ(Progress::LimitExceeded,o.observe_complete_frame(frame(column(),2))); }
    EXPECT_EQ(0u,o.record_count());EXPECT_EQ(Progress::LimitExceeded,o.observe_complete_frame(frame(eof(),3)));
  }
  Observer::Limits limits;limits.max_metadata_name_bytes=7;Observer total(limits);start(total,2);
  ASSERT_EQ(Progress::Collecting,total.observe_complete_frame(frame(column(),2)));
  EXPECT_EQ(Progress::LimitExceeded,total.observe_complete_frame(frame(column(),3)));EXPECT_EQ(1u,total.record_count());
}
TEST(MySqlPrepareNumericObservationTest, SequenceWrapAndValidationBeyondCaptureCapacityRemainBounded) {
  Observer::Limits limits;limits.max_frames=260;limits.max_metadata_entries=1536;Observer o(limits);start(o,254);
  for (unsigned i=0;i<254;++i) { ASSERT_EQ(Progress::Collecting,o.observe_complete_frame(frame(column(),(2+i)&255))); }
  EXPECT_EQ(Progress::Complete,o.observe_complete_frame(frame(eof(),0)));EXPECT_EQ(3u,o.record_count());EXPECT_TRUE(o.records_truncated());
  Observer invalid;start(invalid,4);
  for (unsigned i=0;i<3;++i) { ASSERT_EQ(Progress::Collecting,invalid.observe_complete_frame(frame(column(),2+i))); }
  EXPECT_EQ(Progress::Malformed,invalid.observe_complete_frame(frame({std::byte{251}},5)));EXPECT_EQ(3u,invalid.record_count());EXPECT_TRUE(invalid.records_truncated());
}
}
TEST(MySqlPrepareNumericObservationTest, PrepareHeaderAndWholeFrameBoundariesAreValidatedBeforeCountsPublish) {
  const auto valid=prepare(1,2);
  for (std::size_t n=0;n<valid.size();++n) {
    Observer o;o.begin();EXPECT_EQ(Progress::Malformed,o.observe_complete_frame(frame(Bytes(valid.begin(),valid.begin()+n),1)));
    EXPECT_EQ(0u,o.parameter_count());EXPECT_EQ(0u,o.result_count());EXPECT_EQ(0u,o.record_count());
  }
  for (unsigned fault=0;fault<5;++fault) {
    auto payload=valid;
    if (fault==0) { payload[0]=std::byte{1}; }
    if (fault==1) { payload[9]=std::byte{1}; }
    if (fault==2) { payload.push_back(std::byte{0}); }
    auto complete=frame(payload,fault==3?2:1);
    if (fault==4) { complete.push_back(std::byte{0}); }
    Observer o;o.begin();EXPECT_EQ(Progress::Malformed,o.observe_complete_frame(complete));EXPECT_EQ(0u,o.parameter_count());
  }
  // Nonzero warning/status fields remain structural bytes. Their acceptance
  // here cannot qualify the runtime warning-delivery or transaction policy.
  auto warning=prepare(0,0);warning[10]=std::byte{1};Observer structural;structural.begin();
  EXPECT_EQ(Progress::Complete,structural.observe_complete_frame(frame(warning,1)));EXPECT_EQ(0u,structural.record_count());
}
TEST(MySqlPrepareNumericObservationTest, EverySmallCountPairUsesItsActualStagesAndTruncationFlag) {
  for (unsigned parameters=0;parameters<=4;++parameters) for (unsigned results=0;results<=4;++results) {
    SCOPED_TRACE(parameters);
    SCOPED_TRACE(results);Observer o;o.begin();unsigned seq=1;
    const auto initial=o.observe_complete_frame(frame(prepare(parameters,results),seq++));
    EXPECT_EQ(parameters+results?Progress::Collecting:Progress::Complete,initial);
    EXPECT_EQ(parameters,o.parameter_count());EXPECT_EQ(results,o.result_count());EXPECT_EQ(parameters>3,o.records_truncated());
    for (unsigned i=0;i<parameters;++i) {
      EXPECT_EQ(Progress::Collecting,o.observe_complete_frame(frame(column(12,100+i,6,45),seq++)));
    }
    if (parameters) {
      EXPECT_EQ(results?Progress::Collecting:Progress::Complete,o.observe_complete_frame(frame(eof(),seq++)));
    }
    for (unsigned i=0;i<results;++i) {
      EXPECT_EQ(Progress::Collecting,o.observe_complete_frame(frame(column(8,900+i,0,63),seq++)));
    }
    if (results) { EXPECT_EQ(Progress::Complete,o.observe_complete_frame(frame(eof(),seq++))); }
    EXPECT_EQ(Progress::Complete,o.progress());EXPECT_EQ(std::min(parameters,3u),o.record_count());
    for (unsigned i=0;i<o.record_count();++i) { EXPECT_EQ(100+i,o.records()[i].width);EXPECT_EQ(12u,o.records()[i].type); }
  }
}
TEST(MySqlPrepareNumericObservationTest, EveryPrepareColumnAndEofFullFramePrefixIsTerminalInconclusive) {
  for (unsigned stage=0;stage<3;++stage) {
    const auto complete=stage==0?frame(prepare(),1):(stage==1?frame(column(),2):frame(eof(),3));
    for (std::size_t n=0;n<complete.size();++n) {
      SCOPED_TRACE(stage);
      SCOPED_TRACE(n);Observer o;
      if (stage==0) { o.begin(); }
      else { start(o); }
      if (stage==2) { ASSERT_EQ(Progress::Collecting,o.observe_complete_frame(frame(column(),2))); }
      EXPECT_EQ(Progress::Inconclusive,o.observe_complete_frame(std::span(complete).first(n)));
      EXPECT_EQ(stage==2?1u:0u,o.record_count());
      EXPECT_EQ(Progress::Inconclusive,o.observe_complete_frame(complete));
      o.begin();EXPECT_EQ(0u,o.record_count());
      EXPECT_EQ(Progress::Complete,o.observe_complete_frame(frame(prepare(0,0),1)));
    }
  }
}
TEST(MySqlPrepareNumericObservationTest, ExactResourceBoundariesAndMaximumLimitArithmeticStayRepresentable) {
  for (unsigned boundary=0;boundary<5;++boundary) for (bool exact:{false,true}) {
    Observer::Limits limits;
    const auto wire=frame(prepare(),1).size()+frame(column(),2).size()+frame(eof(),3).size();
    if (boundary==0) { limits.max_wire_bytes=wire-(exact?0:1); }
    if (boundary==1) { limits.max_frames=exact?3:2; }
    if (boundary==2) { limits.max_metadata_entries=exact?6:5; }
    if (boundary==3) { limits.max_metadata_name_bytes=exact?4:3; }
    if (boundary==4) { limits.max_column_name_bytes=exact?1:0; }
    Observer o(limits);o.begin();o.observe_complete_frame(frame(prepare(),1));
    o.observe_complete_frame(frame(column(),2));o.observe_complete_frame(frame(eof(),3));
    EXPECT_EQ(exact?Progress::Complete:Progress::LimitExceeded,o.progress());
    EXPECT_EQ(exact || boundary==0?1u:0u,o.record_count());
  }
  Observer::Limits unlimited;
  unlimited.max_wire_bytes=std::numeric_limits<std::size_t>::max();
  unlimited.max_frames=std::numeric_limits<std::size_t>::max();
  unlimited.max_metadata_entries=std::numeric_limits<std::size_t>::max();
  unlimited.max_metadata_name_bytes=std::numeric_limits<std::size_t>::max();
  unlimited.max_column_name_bytes=std::numeric_limits<std::size_t>::max();
  Observer large_limits(unlimited);start(large_limits);EXPECT_EQ(Progress::Collecting,large_limits.observe_complete_frame(frame(column(255,0xffffffffu,255,65535),2)));
  EXPECT_EQ(Progress::Complete,large_limits.observe_complete_frame(frame(eof(),3)));
  EXPECT_EQ(0xffffffffu,large_limits.records()[0].width);EXPECT_EQ(65535u,large_limits.records()[0].charset);
  // No loop/allocation for UINT16_MAX counts: default limits reject at header.
  Observer huge_count;huge_count.begin();EXPECT_EQ(Progress::LimitExceeded,huge_count.observe_complete_frame(frame(prepare(65535,65535),1)));
  EXPECT_EQ(0u,huge_count.parameter_count());EXPECT_EQ(0u,huge_count.result_count());
  // A maximum advertised 24-bit frame cannot bypass wire limits even if only
  // its four-byte header was supplied; no reassembly or allocation follows.
  Observer advertised;advertised.begin();EXPECT_EQ(Progress::LimitExceeded,advertised.observe_complete_frame(Bytes{std::byte{255},std::byte{255},std::byte{255},std::byte{1}}));
}
TEST(MySqlPrepareNumericObservationTest, AggregateNamesAndResultFramesConsumeBudgetsAfterCaptureCapacity) {
  for (bool exact:{false,true}) {
    Observer::Limits limits;limits.max_metadata_name_bytes=exact?20:19;
    Observer o(limits);start(o,4,1);unsigned seq=2;
    for (unsigned i=0;i<4;++i) { ASSERT_EQ(Progress::Collecting,o.observe_complete_frame(frame(column(),seq++))); }
    ASSERT_EQ(Progress::Collecting,o.observe_complete_frame(frame(eof(),seq++)));
    EXPECT_EQ(exact?Progress::Collecting:Progress::LimitExceeded,o.observe_complete_frame(frame(column(8,19,0,63),seq++)));
    if (exact) { EXPECT_EQ(Progress::Complete,o.observe_complete_frame(frame(eof(),seq))); }
    EXPECT_EQ(3u,o.record_count());EXPECT_TRUE(o.records_truncated());
  }
  Observer::Limits limits;limits.max_metadata_entries=29;
  Observer early(limits);early.begin();EXPECT_EQ(Progress::LimitExceeded,early.observe_complete_frame(frame(prepare(4,1),1)));
  EXPECT_FALSE(early.records_truncated());EXPECT_EQ(0u,early.record_count());
}
TEST(MySqlPrepareNumericObservationTest, TerminalStatesAndOwnedCopiesRemainIsolatedAcrossReset) {
  for (unsigned terminal=0;terminal<4;++terminal) {
    Observer o;start(o,2);auto bytes=frame(column(12,104,6,45),2);
    ASSERT_EQ(Progress::Collecting,o.observe_complete_frame(bytes));const auto owned=o.records();
    const auto copy=o;bytes.assign(bytes.size(),std::byte{0});
    if (terminal==0) { o.missing(); }
    if (terminal==1) { o.observe_complete_frame(frame(column(),4)); }
    if (terminal==2) { o.observe_complete_frame(Bytes{std::byte{255},std::byte{255},std::byte{255},std::byte{3}}); }
    if (terminal==3) { o.observe_complete_frame(frame(column(),3));o.observe_complete_frame(frame(eof(),4)); }
    const auto state=o.progress();EXPECT_NE(Progress::Collecting,state);
    EXPECT_EQ(state,o.observe_complete_frame(frame(prepare(0,0),1)));
    EXPECT_EQ(1u,copy.record_count());EXPECT_EQ(104u,copy.records()[0].width);EXPECT_EQ(45u,owned[0].charset);
    o.begin();EXPECT_EQ(0u,o.record_count());EXPECT_EQ(0u,o.records()[0].width);EXPECT_FALSE(o.records_truncated());
    EXPECT_EQ(Progress::Complete,o.observe_complete_frame(frame(prepare(0,0),1)));
    EXPECT_EQ(12u,owned[0].type);EXPECT_EQ(6u,owned[0].decimals);
  }
}
TEST(MySqlPrepareNumericObservationTest, OverflowLengthPrefixesAndExtraDeclaredColumnsCannotPublishRecords) {
  for (unsigned prefix:{252u,253u,254u}) {
    Bytes payload;integer(payload,prefix,1);integer(payload,std::numeric_limits<std::uint64_t>::max(),prefix==252?2:(prefix==253?3:8));
    Observer o;start(o);EXPECT_EQ(Progress::Malformed,o.observe_complete_frame(frame(payload,2)));EXPECT_EQ(0u,o.record_count());
  }
  for (unsigned count:{0u,1u,2u}) {
    Observer o;start(o,count,1);unsigned seq=2;
    for (unsigned i=0;i<count;++i) { ASSERT_EQ(Progress::Collecting,o.observe_complete_frame(frame(column(),seq++))); }
    // A result column cannot replace the mandatory parameter EOF when count>0.
    if (count) { EXPECT_EQ(Progress::Malformed,o.observe_complete_frame(frame(column(),seq))); }
    else {
      ASSERT_EQ(Progress::Collecting,o.observe_complete_frame(frame(column(),seq++)));
      EXPECT_EQ(Progress::Malformed,o.observe_complete_frame(frame(column(),seq)));
    }
    EXPECT_NE(Progress::Complete,o.progress());
  }
}
namespace {
void same_observation(const Observer& whole,const Observer& split) {
  EXPECT_EQ(whole.progress(),split.progress());EXPECT_EQ(whole.record_count(),split.record_count());
  EXPECT_EQ(whole.parameter_count(),split.parameter_count());EXPECT_EQ(whole.result_count(),split.result_count());
  EXPECT_EQ(whole.records_truncated(),split.records_truncated());
  for (std::size_t i=0;i<3;++i) {
    EXPECT_EQ(whole.records()[i].charset,split.records()[i].charset);EXPECT_EQ(whole.records()[i].type,split.records()[i].type);
    EXPECT_EQ(whole.records()[i].width,split.records()[i].width);EXPECT_EQ(whole.records()[i].decimals,split.records()[i].decimals);
  }
}
}
TEST(MySqlPrepareNumericObservationTest, NumericHeaderOverloadMatchesAllSmallCountExchangesWithoutPacketStorage) {
  for (unsigned parameters=0;parameters<=4;++parameters) for (unsigned results=0;results<=4;++results) {
    Observer whole,split;whole.begin();split.begin();std::uint8_t seq=1;
    auto feed=[&](Bytes payload) {
      const auto size=static_cast<std::uint32_t>(payload.size());
      EXPECT_EQ(whole.observe_complete_frame(frame(payload,seq)),split.observe_complete_payload(size,seq,payload));
      ++seq;same_observation(whole,split);payload.assign(payload.size(),std::byte{0});same_observation(whole,split);
    };
    feed(prepare(parameters,results));
    for (unsigned i=0;i<parameters;++i) { feed(column(12,104+i,6,45)); }
    if (parameters) { feed(eof()); }
    for (unsigned i=0;i<results;++i) { feed(column(8,19,0,63)); }
    if (results) { feed(eof()); }
    EXPECT_EQ(Progress::Complete,split.progress());EXPECT_EQ(parameters>3,split.records_truncated());
  }
}
TEST(MySqlPrepareNumericObservationTest, NumericHeaderPartialOversizedAndSequenceFailuresMatchWholeFramePolicy) {
  for (unsigned fault=0;fault<6;++fault) {
    Observer whole,split;start(whole);start(split);auto payload=column();
    auto size=static_cast<std::uint32_t>(payload.size());std::uint8_t seq=2;
    if (fault==0) { payload.pop_back(); }
    if (fault==1) { payload.push_back(std::byte{0}); }
    if (fault==2) { size=0;payload.clear(); }
    if (fault==3) { seq=3; }
    if (fault==4) { payload.back()=std::byte{1}; }
    if (fault==5) { size=0xffffffu;payload.clear(); }
    Bytes input;integer(input,size,3);integer(input,seq,1);input.insert(input.end(),payload.begin(),payload.end());
    EXPECT_EQ(whole.observe_complete_frame(input),split.observe_complete_payload(size,seq,payload));same_observation(whole,split);
    EXPECT_EQ(fault==0?Progress::Inconclusive:(fault==5?Progress::LimitExceeded:Progress::Malformed),split.progress());
    EXPECT_EQ(0u,split.record_count());
    const auto state=split.progress();EXPECT_EQ(state,split.observe_complete_payload(0,99,{}));
  }
  Observer unrepresentable;unrepresentable.begin();
  EXPECT_EQ(Progress::Malformed,unrepresentable.observe_complete_payload(0x1000000u,1,{}));EXPECT_EQ(0u,unrepresentable.parameter_count());
  Observer::Limits zero;zero.max_frames=0;zero.max_wire_bytes=0;zero.max_metadata_entries=0;
  Observer impossible(zero);impossible.begin();
  EXPECT_EQ(Progress::Malformed,impossible.observe_complete_payload(0x1000000u,1,{}));
  Observer inactive;EXPECT_EQ(Progress::Inconclusive,inactive.observe_complete_payload(12,1,prepare()));
}
TEST(MySqlPrepareNumericObservationTest, NumericHeaderBudgetsAndEveryPayloadPrefixPreserveAtomicRecords) {
  for (unsigned boundary=0;boundary<4;++boundary) for (bool exact:{false,true}) {
    Observer::Limits limits;
    if (boundary==0) { limits.max_wire_bytes=exact?52:51; }
    if (boundary==1) { limits.max_frames=exact?3:2; }
    if (boundary==2) { limits.max_metadata_entries=exact?6:5; }
    if (boundary==3) { limits.max_metadata_name_bytes=exact?4:3; }
    Observer whole(limits),split(limits);whole.begin();split.begin();unsigned seq=1;
    for (auto payload:{prepare(),column(),eof()}) {
      EXPECT_EQ(whole.observe_complete_frame(frame(payload,seq)),split.observe_complete_payload(static_cast<std::uint32_t>(payload.size()),static_cast<std::uint8_t>(seq),payload));
      ++seq;same_observation(whole,split);
    }
    EXPECT_EQ(exact?Progress::Complete:Progress::LimitExceeded,split.progress());
  }
  const auto payload=column();
  for (std::size_t n=0;n<payload.size();++n) {
    Observer split;start(split,2);ASSERT_EQ(Progress::Collecting,split.observe_complete_payload(static_cast<std::uint32_t>(payload.size()),2,payload));
    EXPECT_EQ(Progress::Inconclusive,split.observe_complete_payload(static_cast<std::uint32_t>(payload.size()),3,std::span(payload).first(n)));
    EXPECT_EQ(1u,split.record_count());EXPECT_EQ(104u,split.records()[0].width);
    split.begin();EXPECT_EQ(0u,split.record_count());EXPECT_EQ(Progress::Complete,split.observe_complete_payload(12,1,prepare(0,0)));
  }
}
