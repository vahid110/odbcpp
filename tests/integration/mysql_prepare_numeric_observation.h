#pragma once

#include "core/database/mysql/query_wire.h"
#include <array>

namespace rs::core::database::mysql::integration_detail {
// Diagnostic structure only. Complete means a structurally complete PREPARE
// exchange, never normalized validity, full capture, admission or native proof.
class PrepareNumericObservation {
 public:
  struct RawColumn {
    std::uint16_t charset{};
    std::uint8_t type{};
    std::uint32_t width{};
    std::uint8_t decimals{};
  };
  struct Limits {
    std::size_t max_frames{64};
    std::size_t max_wire_bytes{65536};
    std::size_t max_metadata_entries{384};
    std::size_t max_metadata_name_bytes{4096};
    std::size_t max_column_name_bytes{1024};
  };
  enum class Progress { Inconclusive, Collecting, Complete, Malformed, LimitExceeded };
  PrepareNumericObservation() = default;
  explicit PrepareNumericObservation(Limits limits) : limits_(limits) {}
  // Future owner must pair begin with an actual complete command22. Never infer
  // PREPARE ownership from a response OK or reconstruct fragmented packets.
  void begin() noexcept {
    records_={};record_count_=0;parameter_count_=0;result_count_=0;
    records_truncated_=false;frames_=0;wire_bytes_=0;names_=0;entries_=0;
    remaining_=0;sequence_=1;stage_=Stage::Prepare;progress_=Progress::Collecting;
  }
  void missing() noexcept {
    if (progress_==Progress::Collecting) progress_=Progress::Inconclusive;
  }
  Progress progress() const noexcept { return progress_; }
  const std::array<RawColumn,3>& records() const noexcept { return records_; }
  std::size_t record_count() const noexcept { return record_count_; }
  std::uint16_t parameter_count() const noexcept { return parameter_count_; }
  std::uint16_t result_count() const noexcept { return result_count_; }
  bool records_truncated() const noexcept { return records_truncated_; }
  Progress observe_complete_frame(std::span<const std::byte> frame) {
    if (progress_!=Progress::Collecting) return progress_;
    if (frame.size()<4) return stop(Progress::Inconclusive);
    query_detail::Cursor header(frame.first(4));std::uint64_t size{},sequence{};
    header.integer(3,size);header.integer(1,sequence);
    return observe_complete_payload(static_cast<std::uint32_t>(size),static_cast<std::uint8_t>(sequence),frame.subspan(4));
  }
  // Header numbers must be actual wire observations; this overload grants no
  // exchange ownership. Payload is borrowed synchronously, never retained.
  Progress observe_complete_payload(std::uint32_t size,std::uint8_t sequence,
                                    std::span<const std::byte> payload) {
    if (progress_!=Progress::Collecting) return progress_;
    if (size>0xffffffu) return stop(Progress::Malformed);
    const auto wire_size=static_cast<std::uint64_t>(size)+4;
    if (frames_>=limits_.max_frames || wire_bytes_>limits_.max_wire_bytes ||
        wire_size>limits_.max_wire_bytes-wire_bytes_) return stop(Progress::LimitExceeded);
    if (size>payload.size()) return stop(Progress::Inconclusive);
    if (!size || size!=payload.size() || sequence!=sequence_) return stop(Progress::Malformed);
    ++frames_;wire_bytes_+=static_cast<std::size_t>(wire_size);++sequence_;
    if (stage_==Stage::Prepare) return prepare(payload);
    if (stage_==Stage::Parameter || stage_==Stage::Result) {
      RawColumn raw{};std::size_t names{};
      const auto parsed=column(payload,raw,names);
      if (parsed!=Progress::Collecting) return stop(parsed);
      names_+=names;entries_+=6;
      if (stage_==Stage::Parameter && record_count_<records_.size()) records_[record_count_++]=raw;
      if (--remaining_==0) stage_=stage_==Stage::Parameter?Stage::ParameterEof:Stage::ResultEof;
      return progress_;
    }
    if (payload.size()!=5 || payload[0]!=std::byte{254}) return stop(Progress::Malformed);
    if (stage_==Stage::ParameterEof && result_count_) {
      stage_=Stage::Result;remaining_=result_count_;return progress_;
    }
    return stop(Progress::Complete);
  }
 private:
  enum class Stage { Prepare, Parameter, ParameterEof, Result, ResultEof };
  Progress stop(Progress value) noexcept { progress_=value;return value; }
  Progress prepare(std::span<const std::byte> payload) {
    if (payload.size()!=12 || payload[0]!=std::byte{0} || payload[9]!=std::byte{0})
      return stop(Progress::Malformed);
    query_detail::Cursor c(payload);std::uint64_t tag{},id{},columns{},parameters{};
    c.integer(1,tag);c.integer(4,id);c.integer(2,columns);c.integer(2,parameters);
    const auto count=parameters+columns;
    const auto required_frames=1+count+(parameters?1:0)+(columns?1:0);
    if (count>limits_.max_metadata_entries/6 || required_frames>limits_.max_frames)
      return stop(Progress::LimitExceeded);
    parameter_count_=static_cast<std::uint16_t>(parameters);result_count_=static_cast<std::uint16_t>(columns);
    records_truncated_=parameters>records_.size();
    if (parameters) { stage_=Stage::Parameter;remaining_=parameter_count_; }
    else if (columns) { stage_=Stage::Result;remaining_=result_count_; }
    else { return stop(Progress::Complete); }
    return progress_;
  }
  Progress column(std::span<const std::byte> payload,RawColumn& raw,std::size_t& names) const {
    query_detail::Cursor c(payload);std::string_view fields[6];std::size_t count{};
    for (auto& field:fields) {
      if (!c.text(field)) return Progress::Malformed;
      if (names_>limits_.max_metadata_name_bytes || count>limits_.max_metadata_name_bytes-names_ ||
          field.size()>limits_.max_metadata_name_bytes-names_-count) return Progress::LimitExceeded;
      count+=field.size();
    }
    std::uint64_t fixed{},charset{},width{},type{},flags{},decimals{},filler{};
    if (fields[0]!="def" || !c.length(fixed) || fixed!=12 || !c.integer(2,charset) || !c.integer(4,width) ||
        !c.integer(1,type) || !c.integer(2,flags) || !c.integer(1,decimals) || !c.integer(2,filler) ||
        filler || c.remaining()) return Progress::Malformed;
    if (fields[4].size()>limits_.max_column_name_bytes || entries_>limits_.max_metadata_entries ||
        limits_.max_metadata_entries-entries_<6) return Progress::LimitExceeded;
    if (fields[4].find('\0')!=std::string_view::npos || !rs::util::utf8_code_point_count(fields[4]))
      return Progress::Malformed;
    raw={static_cast<std::uint16_t>(charset),static_cast<std::uint8_t>(type),
         static_cast<std::uint32_t>(width),static_cast<std::uint8_t>(decimals)};
    names=count;return Progress::Collecting;
  }
  Limits limits_{};
  std::array<RawColumn,3> records_{};
  std::size_t record_count_{},frames_{},wire_bytes_{},names_{},entries_{};
  std::uint16_t parameter_count_{},result_count_{},remaining_{};
  std::uint8_t sequence_{1};bool records_truncated_{};
  Stage stage_{Stage::Prepare};Progress progress_{Progress::Inconclusive};
};
}
