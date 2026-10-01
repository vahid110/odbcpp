#include <gtest/gtest.h>
#include "core/database/backend_provider.h"

#include "core/database/generic_database_connection.h"
#include "core/database/database_factory.h"
#include "core/database/postgres/pg_protocol_parser.h"
#include "core/database/postgres/pg_database_connection.h"
#include "core/transport/i_transport.h"
#include "core/transport/start_tls_transport.h"
#include "core/transport/tls_configurable_transport.h"
#include "tests/mock_protocol_parser.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

class FailingQueryTransport final : public rs::core::transport::ITransport {
 public:
  rs::util::Result<void> connect(std::string_view, uint16_t,
                                 rs::util::Deadline) override {
    return {};
  }

  rs::util::Result<rs::core::transport::IOResult> send(
      std::span<const std::byte> buffer, rs::util::Deadline) override {
    if (send_count_++ != 0) {
      return {rs::util::DbErrorCode::NetworkError,
              "injected connection loss"};
    }
    return rs::core::transport::IOResult{buffer.size(), false};
  }

  rs::util::Result<rs::core::transport::IOResult> recv(
      std::span<std::byte> buffer, rs::util::Deadline) override {
    static constexpr std::byte startup[]{
        std::byte{'R'}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{8}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{'Z'}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{4}};
    const auto available = std::size(startup) - receive_offset_;
    const auto count = std::min(buffer.size(), available);
    std::copy_n(startup + receive_offset_, count, buffer.begin());
    receive_offset_ += count;
    return rs::core::transport::IOResult{count, false};
  }

  void close() noexcept override { ++close_count_; }
  std::size_t close_count() const noexcept { return close_count_; }

 private:
  std::size_t send_count_{0};
  std::size_t receive_offset_{0};
  std::size_t close_count_{0};
};

class ScriptedTlsTransport final : public rs::core::transport::ITransport,
                                   public rs::core::transport::IStartTlsTransport,
                                   public rs::core::transport::ITlsConfigurableTransport {
 public:
  enum class Mode {
    Refused, OverreportedWrite, NoProgress, Eof, EofWithAccept,
    InvalidReply, OverreportedRead, InvalidConnect, TimedOutConnect,
    FailedConnect
  };

  explicit ScriptedTlsTransport(Mode mode = Mode::Refused) : mode_(mode) {}

  rs::util::Result<void> connect(std::string_view, uint16_t,
                                 rs::util::Deadline) override {
    if (mode_ == Mode::InvalidConnect) {
      return {rs::util::DbErrorCode::InvalidParameter,
              "injected invalid transport endpoint"};
    }
    if (mode_ == Mode::TimedOutConnect) {
      return {rs::util::DbErrorCode::Timeout,
              "injected transport timeout"};
    }
    if (mode_ == Mode::FailedConnect) {
      return {rs::util::DbErrorCode::NetworkError,
              "injected transport failure"};
    }
    return {};
  }

  rs::util::Result<void> connect_plain(std::string_view host, uint16_t port,
                                       rs::util::Deadline deadline) override {
    return connect(host, port, deadline);
  }

  rs::util::Result<void> upgrade_to_tls(
      std::string_view, rs::util::Deadline) override {
    return {rs::util::DbErrorCode::TLSError,
            "unexpected TLS upgrade after refusal"};
  }

  void set_ca_locations(const std::string& file,
                        const std::string& directory) override {
    ca_file_ = file;
    ca_directory_ = directory;
  }

  rs::util::Result<rs::core::transport::IOResult> send(
      std::span<const std::byte> buffer, rs::util::Deadline) override {
    return rs::core::transport::IOResult{
        buffer.size() + static_cast<std::size_t>(mode_ == Mode::OverreportedWrite),
        false};
  }

  rs::util::Result<rs::core::transport::IOResult> recv(
      std::span<std::byte> buffer, rs::util::Deadline) override {
    if (mode_ == Mode::NoProgress) {
      return rs::core::transport::IOResult{0, false};
    }
    if (mode_ == Mode::Eof) {
      return rs::core::transport::IOResult{0, true};
    }
    if (mode_ == Mode::EofWithAccept) {
      buffer[0] = std::byte{'S'};
      return rs::core::transport::IOResult{1, true};
    }
    if (mode_ == Mode::InvalidReply) {
      buffer[0] = std::byte{'?'};
      return rs::core::transport::IOResult{1, false};
    }
    if (mode_ == Mode::OverreportedRead) {
      buffer[0] = std::byte{'S'};
      return rs::core::transport::IOResult{2, false};
    }
    buffer[0] = std::byte{'N'};
    return rs::core::transport::IOResult{1, false};
  }

  void close() noexcept override { ++close_count_; }
  std::size_t close_count() const noexcept { return close_count_; }
  const std::string& ca_file() const noexcept { return ca_file_; }
  const std::string& ca_directory() const noexcept { return ca_directory_; }

 private:
  Mode mode_;
  std::size_t close_count_{0};
  std::string ca_file_;
  std::string ca_directory_;
};

class ScriptedBackendTransport final : public rs::core::transport::ITransport {
 public:
  enum class ResponseMode {
    ValidStartup, MalformedStartup, MalformedAuth, AuthRejected,
    CleartextAuthentication,
    AuthenticationTimeout,
    MalformedQuery, MalformedStartupReady, MalformedQueryReady,
    MalformedQueryError, MalformedBackendKey, ReadyWithoutAuth,
    StartupRejectedAfterAuth, LoginRejectedAfterAuth,
    DuplicateAuthenticationOk, ChallengeAfterAuthenticationOk,
    ScramOkWithoutServerFinal, ScramDowngradeToCleartext,
    ParameterStatusBeforeAuthenticationOk, BackendKeyBeforeAuthenticationOk,
    UnexpectedQueryFrameBeforeAuthenticationOk,
    UnexpectedQueryFrameAfterAuthenticationOk, NoticeAfterAuthenticationOk,
    MalformedNoticeMissingField, MalformedNoticeDuplicateField,
    MalformedNoticeUnterminated, QueryParameterStatus,
    MalformedQueryParameterStatus, OversizedStartupReady,
    TooLargeDataRow, IncompleteLargeDataRow,
    ZeroHeaderRead, ZeroBodyRead,
    OverreportedHeaderRead, OverreportedStartupWrite,
    AuthenticationDuringQuery, BackendKeyDuringQuery,
    MismatchedDataRow, UnannouncedDataRow, DuplicateRowDescription,
    UnknownQueryFrame, ExtendedFrameDuringSimpleQuery, ResultAfterError,
    ValidNotificationDuringQuery, MalformedNotificationDuringQuery,
    MalformedEmptyQueryResponse,
    CopyInDuringQuery, CopyOutDuringQuery, CopyBothDuringQuery,
    UnsolicitedCopyData, UnsolicitedCopyDone,
    OversizedUnsolicitedCopyData, BinaryResultRow,
    BinaryAdditionalResultRow, ReadyOnlyQuery, RowsWithoutCompletion,
    EmptyQueryResponse, DescriptionNoData, DescriptionOneParameter, DescriptionRepeatedParameter, DescriptionOneColumn,
    DescriptionMissingParse, DescriptionMissingParameters,
    DescriptionMissingResult, DescriptionOutOfOrder,
    DescriptionServerError, Utf8ColumnNames, MalformedColumnName, MalformedAdditionalColumnName, Utf8TextCells, NativeCells, MalformedNativeCells, OwnedResultCells, OwnedTwoResultSets, OwnedResultCellsTransaction, OwnedResultCellsAborted,
    OwnedErrorIdle, OwnedErrorTransaction, OwnedErrorAborted,
    UnterminatedColumnName, TruncatedColumnMetadata, TrailingColumnMetadata, EmptyColumnName,
    QueryReadTimeout, PartialQueryWrite, PreparedCommand, TransactionCompletions, QueryAllocationFailure, Md5Authentication
  };

  explicit ScriptedBackendTransport(
      ResponseMode mode = ResponseMode::ValidStartup,
      char extended_query_tag = '1') : mode_(mode) {
    if (mode == ResponseMode::MalformedStartup) {
      append_message('S', "missing-terminators", 19);
      return;
    }
    if (mode == ResponseMode::MalformedAuth) {
      append_message('R', "\0\0\0", 3);
      return;
    }
    if (mode == ResponseMode::AuthRejected) {
      constexpr char error[] =
          "SFATAL\0C28P01\0Mpassword authentication failed\0";
      append_message('E', error, sizeof(error));
      return;
    }
    if (mode == ResponseMode::Md5Authentication) {
      append_message('R', "\0\0\0\5\1\2\3\4", 8);
    }
    if (mode == ResponseMode::CleartextAuthentication) {
      append_message('R', "\0\0\0\3", 4);
      return;
    }
    if (mode == ResponseMode::AuthenticationTimeout) {
      return;
    }
    if (mode == ResponseMode::MalformedStartupReady) {
      append_message('Z', "X", 1);
      return;
    }
    if (mode == ResponseMode::OversizedStartupReady) {
      input_ = {std::byte{'Z'}, std::byte{0}, std::byte{0},
                std::byte{0x75}, std::byte{0x31}};
      return;
    }
    if (mode == ResponseMode::TooLargeDataRow ||
        mode == ResponseMode::IncompleteLargeDataRow) {
      input_ = {std::byte{'D'}, std::byte{0x40}, std::byte{0},
                std::byte{0}, std::byte{0}};
      if (mode == ResponseMode::IncompleteLargeDataRow) {
        input_[1] = std::byte{0x3f};
        input_[2] = std::byte{0xff};
        input_[3] = std::byte{0xff};
        input_[4] = std::byte{0xfe};
      }
      return;
    }
    if (mode == ResponseMode::ReadyWithoutAuth) {
      append_message('Z', "I", 1);
      return;
    }
    if (mode == ResponseMode::ScramOkWithoutServerFinal ||
        mode == ResponseMode::ScramDowngradeToCleartext) {
      constexpr char offer[] = "\0\0\0\nSCRAM-SHA-256\0";
      append_message('R', offer, sizeof(offer));
      append_message('R', mode == ResponseMode::ScramOkWithoutServerFinal
                              ? "\0\0\0\0" : "\0\0\0\3", 4);
      append_message('Z', "I", 1);
      return;
    }
    if (mode == ResponseMode::ParameterStatusBeforeAuthenticationOk) {
      append_message('S', "server_version\0" "17.6\0", 20);
      return;
    }
    if (mode == ResponseMode::BackendKeyBeforeAuthenticationOk) {
      constexpr char backend_key[] = "\0\0\0\0\0\0\0\0";
      append_message('K', backend_key, 8);
      return;
    }
    if (mode == ResponseMode::UnexpectedQueryFrameBeforeAuthenticationOk) {
      append_message('C', "SELECT 1\0", 9);
      return;
    }
    append_message('R', "\0\0\0\0", 4);
    if (mode == ResponseMode::UnexpectedQueryFrameAfterAuthenticationOk) {
      append_message('C', "SELECT 1\0", 9);
      return;
    }
    if (mode == ResponseMode::NoticeAfterAuthenticationOk) {
      constexpr char notice[] = "SNOTICE\0C00000\0Mstartup notice\0";
      append_message('N', notice, sizeof(notice));
    }
    if (mode == ResponseMode::MalformedNoticeMissingField) {
      constexpr char notice[] = "SNOTICE\0C00000\0\0";
      append_message('N', notice, sizeof(notice) - 1);
      return;
    }
    if (mode == ResponseMode::MalformedNoticeDuplicateField) {
      constexpr char notice[] =
          "SNOTICE\0C00000\0Mstartup notice\0SNOTICE\0\0";
      append_message('N', notice, sizeof(notice) - 1);
      return;
    }
    if (mode == ResponseMode::MalformedNoticeUnterminated) {
      constexpr char notice[] = "SNOTICE\0C00000\0Mstartup notice";
      append_message('N', notice, sizeof(notice) - 1);
      return;
    }
    if (mode == ResponseMode::DuplicateAuthenticationOk) {
      append_message('R', "\0\0\0\0", 4);
      return;
    }
    if (mode == ResponseMode::ChallengeAfterAuthenticationOk) {
      append_message('R', "\0\0\0\3", 4);
      return;
    }
    if (mode == ResponseMode::StartupRejectedAfterAuth) {
      constexpr char error[] =
          "SERROR\0C22023\0Mstartup option rejected\0";
      append_message('E', error, sizeof(error));
      return;
    }
    if (mode == ResponseMode::LoginRejectedAfterAuth) {
      constexpr char error[] =
          "SFATAL\0C28000\0Mrole does not exist\0";
      append_message('E', error, sizeof(error));
      return;
    }
    append_message('S', "server_version\0" "17.6\0", 20);
    append_message('S', "application_name\0" "odbcpp\0", 24);
    constexpr char backend_key[] = "\0\0\0\0\0\0\0\0";
    append_message('K', backend_key,
                   mode == ResponseMode::MalformedBackendKey ? 7 : 8);
    append_message('Z', "I", 1);
    if (mode == ResponseMode::MalformedQuery) {
      append_message('T', "\0\1", 2);
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::MalformedQueryReady) {
      append_message('Z', "IT", 2);
    } else if (mode == ResponseMode::MalformedEmptyQueryResponse) {
      append_message('I', "x", 1);
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::MalformedQueryError) {
      constexpr char error[] = "SERROR\0C42601\0\0";
      append_message('E', error, sizeof(error) - 1);
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::QueryParameterStatus) {
      constexpr char status[] = "application_name\0changed\0";
      append_message('S', status, sizeof(status) - 1);
      append_message('C', "SET\0", 4);
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::MalformedQueryParameterStatus) {
      constexpr char status[] = "application_name\0unterminated";
      append_message('S', status, sizeof(status) - 1);
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::AuthenticationDuringQuery ||
               mode == ResponseMode::BackendKeyDuringQuery) {
      if (mode == ResponseMode::AuthenticationDuringQuery) {
        append_message('R', "\0\0\0\0", 4);
      } else {
        constexpr char query_backend_key[] = "\0\0\0\0\0\0\0\0";
        append_message('K', query_backend_key, 8);
      }
      append_message('C', "SELECT 1\0", 9);
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::MismatchedDataRow) {
      constexpr char description[] =
          "\0\1" "v\0" "\0\0\0\0" "\0\0" "\0\0\0\27"
          "\0\4" "\377\377\377\377" "\0\0";
      append_message('T', description, sizeof(description) - 1);
      append_message('D', "\0\0", 2);
      append_message('C', "SELECT 0", sizeof("SELECT 0"));
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::UnannouncedDataRow) {
      constexpr char row[] = "\0\1\0\0\0\1" "x";
      append_message('D', row, sizeof(row) - 1);
      append_message('C', "SELECT 1", sizeof("SELECT 1"));
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::DuplicateRowDescription) {
      constexpr char first[] =
          "\0\1" "a\0" "\0\0\0\0" "\0\0" "\0\0\0\27"
          "\0\4" "\377\377\377\377" "\0\0";
      constexpr char second[] =
          "\0\1" "b\0" "\0\0\0\0" "\0\0" "\0\0\0\27"
          "\0\4" "\377\377\377\377" "\0\0";
      append_message('T', first, sizeof(first) - 1);
      append_message('T', second, sizeof(second) - 1);
      append_message('C', "SELECT 0", sizeof("SELECT 0"));
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::UnknownQueryFrame) {
      append_message('?', "", 0);
      append_message('C', "SELECT 0", sizeof("SELECT 0"));
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::ExtendedFrameDuringSimpleQuery) {
      if (extended_query_tag == 't') {
        append_message('t', "\0\0", 2);
      } else {
        append_message(extended_query_tag, "", 0);
      }
      append_message('C', "SELECT 0", sizeof("SELECT 0"));
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::ResultAfterError) {
      constexpr char error[] = "SERROR\0C22012\0Mdivision by zero\0";
      append_message('C', "SELECT 1", sizeof("SELECT 1"));
      append_message('E', error, sizeof(error));
      append_message('C', "SELECT 1", sizeof("SELECT 1"));
      append_message('Z', "E", 1);
    } else if (mode == ResponseMode::ValidNotificationDuringQuery ||
               mode == ResponseMode::MalformedNotificationDuringQuery) {
      constexpr char valid[] = "\0\0\0\1" "channel\0" "payload\0";
      append_message('A', valid,
                     mode == ResponseMode::ValidNotificationDuringQuery
                         ? sizeof(valid) - 1 : sizeof(valid) - 2);
      append_message('C', "SELECT 0", sizeof("SELECT 0"));
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::CopyInDuringQuery ||
               mode == ResponseMode::CopyOutDuringQuery ||
               mode == ResponseMode::CopyBothDuringQuery) {
      const char tag = mode == ResponseMode::CopyInDuringQuery ? 'G' :
                       mode == ResponseMode::CopyOutDuringQuery ? 'H' : 'W';
      append_message(tag, "\0\0\0", 3);
    } else if (mode == ResponseMode::UnsolicitedCopyData) {
      append_message('d', "data", 4);
      append_message('C', "SELECT 0", sizeof("SELECT 0"));
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::UnsolicitedCopyDone) {
      append_message('c', "", 0);
      append_message('C', "SELECT 0", sizeof("SELECT 0"));
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::OversizedUnsolicitedCopyData) {
      input_.insert(input_.end(), {
          std::byte{'d'}, std::byte{0x3f}, std::byte{0xff},
          std::byte{0xff}, std::byte{0xfe}});
    } else if (mode == ResponseMode::BinaryResultRow ||
               mode == ResponseMode::BinaryAdditionalResultRow) {
      constexpr char description[] =
          "\0\1" "value\0" "\0\0\0\0" "\0\0" "\0\0\0\27"
          "\0\4" "\377\377\377\377" "\0\1";
      constexpr char row[] = "\0\1" "\0\0\0\4" "\0\0\0\52";
      if (mode == ResponseMode::BinaryAdditionalResultRow) {
        append_message('C', "UPDATE 0", sizeof("UPDATE 0"));
      }
      append_message('T', description, sizeof(description) - 1);
      append_message('D', row, sizeof(row) - 1);
      append_message('C', "SELECT 1", sizeof("SELECT 1"));
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::ReadyOnlyQuery) {
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::RowsWithoutCompletion) {
      constexpr char description[] =
          "\0\1" "value\0" "\0\0\0\0" "\0\0" "\0\0\0\27"
          "\0\4" "\377\377\377\377" "\0\0";
      constexpr char row[] = "\0\1" "\0\0\0\1" "7";
      append_message('T', description, sizeof(description) - 1);
      append_message('D', row, sizeof(row) - 1);
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::EmptyQueryResponse) {
      append_message('I', "", 0);
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::TransactionCompletions) {
      append_message('C', "BEGIN", sizeof("BEGIN")); append_message('Z', "T", 1);
      append_message('C', "COMMIT", sizeof("COMMIT")); append_message('Z', "I", 1);
      append_message('C', "ROLLBACK", sizeof("ROLLBACK")); append_message('Z', "I", 1);
      append_message('C', "SET", sizeof("SET")); append_message('Z', "I", 1);
    } else if (mode == ResponseMode::PreparedCommand) {
      append_message('1', "", 0);
      constexpr char parameters[] = "\0\4\0\0\0\31\0\0\0\31\0\0\0\31\0\0\0\21";
      append_message('t', parameters, sizeof(parameters) - 1);
      append_message('2', "", 0);
      append_message('n', "", 0);
      append_message('C', "UPDATE 1", sizeof("UPDATE 1"));
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::DescriptionNoData) {
      append_message('1', "", 0);
      append_message('t', "\0\0", 2);
      append_message('n', "", 0);
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::DescriptionOneColumn) {
      append_message('1', "", 0);
      append_message('t', "\0\0", 2);
      constexpr char description[] =
          "\0\1value\0" "\0\0\0\0" "\0\0" "\0\0\0\31"
          "\377\377" "\377\377\377\377" "\0\0";
      append_message('T', description, sizeof(description) - 1);
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::DescriptionOneParameter || mode == ResponseMode::DescriptionRepeatedParameter) {
      for (int response = 0; response < (mode == ResponseMode::DescriptionRepeatedParameter ? 2 : 1); ++response) {
        append_message('1', "", 0);
        constexpr char parameters[] = "\0\1\0\0\0\27";
        append_message('t', parameters, sizeof(parameters) - 1);
        append_message('n', "", 0);
        append_message('Z', "I", 1);
      }
    } else if (mode == ResponseMode::DescriptionMissingParse) {
      append_message('t', "\0\0", 2);
      append_message('n', "", 0);
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::DescriptionMissingParameters) {
      append_message('1', "", 0);
      append_message('n', "", 0);
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::DescriptionMissingResult) {
      append_message('1', "", 0);
      append_message('t', "\0\0", 2);
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::DescriptionOutOfOrder) {
      append_message('t', "\0\0", 2);
      append_message('1', "", 0);
      append_message('n', "", 0);
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::UnterminatedColumnName || mode == ResponseMode::TruncatedColumnMetadata ||
               mode == ResponseMode::TrailingColumnMetadata || mode == ResponseMode::EmptyColumnName) {
      constexpr char description[] =
          "\0\1\0" "\0\0\0\0" "\0\0" "\0\0\0\31"
          "\377\377" "\377\377\377\377" "\0\0";
      if (mode == ResponseMode::UnterminatedColumnName) append_message('T', "\0\1value", 7);
      else if (mode == ResponseMode::TruncatedColumnMetadata) append_message('T', description, sizeof(description) - 2);
      else append_message('T', description, sizeof(description) - (mode == ResponseMode::EmptyColumnName ? 1 : 0));
      append_message('C', "SELECT 0", sizeof("SELECT 0"));
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::Utf8ColumnNames || mode == ResponseMode::MalformedColumnName ||
               mode == ResponseMode::MalformedAdditionalColumnName) {
      constexpr char metadata[] = "\0\0\0\0\0\0\0\0\0\31\377\377\377\377\377\377\0\0";
      for (int result = 0; result < 2; ++result) {
        const bool malformed = (mode == ResponseMode::MalformedColumnName && result == 0) ||
            (mode == ResponseMode::MalformedAdditionalColumnName && result == 1);
        const std::string name = malformed ? "\xed\xa0\x80" : "\xe2\x82\xac\xf0\x9f\x98\x80";
        std::string description("\0\1", 2); description += name; description.push_back('\0');
        description.append(metadata, sizeof(metadata) - 1);
        append_message('T', description.data(), description.size());
        constexpr char row[] = "\0\1\0\0\0\1x";
        append_message('D', row, sizeof(row) - 1);
        append_message('C', "SELECT 1", sizeof("SELECT 1"));
      }
      append_message('Z', "I", 1);
      append_message('C', "SELECT 0", sizeof("SELECT 0"));
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::Utf8TextCells) {
      constexpr char description[] =
          "\0\3fixed\0" "\0\0\0\0" "\0\0" "\0\0\4\22" "\377\377" "\377\377\377\377" "\0\0"
          "varying\0" "\0\0\0\0" "\0\0" "\0\0\4\23" "\377\377" "\377\377\377\377" "\0\0"
          "text\0" "\0\0\0\0" "\0\0" "\0\0\0\31" "\377\377" "\377\377\377\377" "\0\0";
      const std::vector<std::optional<std::string>> values{
          "ascii", "", std::nullopt, "\xe2\x82\xac\xf0\x9f\x98\x80",
          std::string("a\0\xf4\x8f\xbf\xbf", 6), "\xc0\x80", "\xed\xa0\x80",
          "\xf4\x90\x80\x80", "\xe2\x82", "\x80"};
      for (int result = 0; result < 2; ++result) {
        append_message('T', description, sizeof(description) - 1);
        for (const auto& value : values) {
          std::string payload("\0\3", 2);
          for (int column = 0; column < 3; ++column) {
            const auto length = value ? static_cast<std::uint32_t>(value->size()) : UINT32_MAX;
            for (const int shift : {24, 16, 8, 0}) payload.push_back(static_cast<char>((length >> shift) & 0xff));
            if (value) payload += *value;
          }
          append_message('D', payload.data(), payload.size());
        }
        append_message('C', "SELECT 10", sizeof("SELECT 10"));
      }
      append_message('Z', "I", 1);
      // A later exchange proves malformed text did not corrupt framing/liveness.
      append_message('C', "SELECT 0", sizeof("SELECT 0"));
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::NativeCells || mode == ResponseMode::MalformedNativeCells) {
      constexpr char description[] =
          "\0\2octets\0" "\0\0\0\0" "\0\0" "\0\0\0\21" "\377\377" "\377\377\377\377" "\0\0"
          "flag\0" "\0\0\0\0" "\0\0" "\0\0\0\20" "\0\1" "\377\377\377\377" "\0\0";
      constexpr char valid[] = "\0\2\0\0\0\10\\x00ff5c\0\0\0\1t";
      constexpr char malformed[] = "\0\2\0\0\0\4\\xzz\0\0\0\1?";
      constexpr char nulls[] = "\0\2\377\377\377\377\377\377\377\377";
      constexpr char empty[] = "\0\2\0\0\0\2\\x\0\0\0\1f";
      for (int result = 0; result < 2; ++result) {
        append_message('T', description, sizeof(description) - 1);
        if (mode == ResponseMode::MalformedNativeCells) append_message('D', malformed, sizeof(malformed) - 1);
        else append_message('D', valid, sizeof(valid) - 1);
        append_message('D', nulls, sizeof(nulls) - 1);
        append_message('D', empty, sizeof(empty) - 1);
        append_message('C', "SELECT 3", sizeof("SELECT 3"));
      }
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::OwnedResultCells ||
               mode == ResponseMode::OwnedResultCellsTransaction || mode == ResponseMode::OwnedResultCellsAborted ||
               mode == ResponseMode::OwnedTwoResultSets) {
      constexpr char description[] =
          "\0\1value\0" "\0\0\0\0" "\0\0" "\0\0\0\31"
          "\377\377" "\377\377\377\377" "\0\0";
      append_message('T', description, sizeof(description) - 1);
      constexpr char null_row[] = "\0\1\377\377\377\377";
      constexpr char empty_row[] = "\0\1\0\0\0\0";
      constexpr char text_row[] = "\0\1\0\0\0\3abc";
      append_message('D', null_row, sizeof(null_row) - 1);
      append_message('D', empty_row, sizeof(empty_row) - 1);
      append_message('D', text_row, sizeof(text_row) - 1);
      append_message('C', "SELECT 3", sizeof("SELECT 3"));
      if (mode == ResponseMode::OwnedTwoResultSets) {
        append_message('T', description, sizeof(description) - 1);
        append_message('D', null_row, sizeof(null_row) - 1);
        append_message('D', empty_row, sizeof(empty_row) - 1);
        append_message('D', text_row, sizeof(text_row) - 1);
        append_message('C', "SELECT 3", sizeof("SELECT 3"));
      } else {
        constexpr char error[] = "SERROR\0C22012\0Mdivision by zero\0";
        append_message('E', error, sizeof(error));
      }
      const char state = (mode == ResponseMode::OwnedResultCells || mode == ResponseMode::OwnedTwoResultSets) ? 'I' :
          mode == ResponseMode::OwnedResultCellsTransaction ? 'T' : 'E';
      append_message('Z', &state, 1);
    } else if (mode == ResponseMode::OwnedErrorIdle ||
               mode == ResponseMode::OwnedErrorTransaction ||
               mode == ResponseMode::OwnedErrorAborted) {
      constexpr char error[] = "SERROR\0C42601\0Msyntax error\0";
      append_message('E', error, sizeof(error));
      const char state = mode == ResponseMode::OwnedErrorIdle ? 'I' :
                         mode == ResponseMode::OwnedErrorTransaction ? 'T' : 'E';
      append_message('Z', &state, 1);
      append_message('C', "ROLLBACK", sizeof("ROLLBACK"));
      append_message('Z', "I", 1);
    } else if (mode == ResponseMode::DescriptionServerError) {
      constexpr char error[] = "SERROR\0C42601\0Msyntax error\0";
      append_message('E', error, sizeof(error));
      append_message('Z', "I", 1);
    }
  }

  rs::util::Result<void> connect(std::string_view, uint16_t,
                                 rs::util::Deadline) override {
    ++connect_count_;
    return {};
  }

  rs::util::Result<rs::core::transport::IOResult> send(
      std::span<const std::byte> buffer, rs::util::Deadline) override {
    ++send_count_;
    if (mode_ == ResponseMode::PartialQueryWrite && send_count_ > 1) {
      if (send_count_ == 2) return rs::core::transport::IOResult{1, false};
      return {rs::util::DbErrorCode::NetworkError, "injected partial query write"};
    }
    if (mode_ == ResponseMode::OverreportedStartupWrite) {
      return rs::core::transport::IOResult{buffer.size() + 1, false};
    }
    return rs::core::transport::IOResult{buffer.size(), false};
  }

  std::size_t send_count() const noexcept { return send_count_; }
  std::size_t connect_count() const noexcept { return connect_count_; }
  std::size_t close_count() const noexcept { return close_count_; }
  std::size_t bytes_read() const noexcept { return offset_; }

  rs::util::Result<rs::core::transport::IOResult> recv(
      std::span<std::byte> buffer, rs::util::Deadline) override {
    if (mode_ == ResponseMode::QueryAllocationFailure && send_count_ > 1) {
      throw std::bad_alloc{};
    }
    if (mode_ == ResponseMode::AuthenticationTimeout) {
      return {rs::util::DbErrorCode::Timeout,
              "injected authentication timeout"};
    }
    if ((mode_ == ResponseMode::ZeroHeaderRead && offset_ == 0) ||
        (mode_ == ResponseMode::ZeroBodyRead && offset_ == 5)) {
      if (!zero_returned_) {
        zero_returned_ = true;
        return rs::core::transport::IOResult{0, false};
      }
      return {rs::util::DbErrorCode::NetworkError,
              "injected repeated read"};
    }
    const auto available = input_.size() - offset_;
    if (mode_ == ResponseMode::QueryReadTimeout && available == 0) {
      return {rs::util::DbErrorCode::Timeout, "injected query read timeout"};
    }
    if (available == 0) return rs::core::transport::IOResult{0, true};
    const auto count = std::min(buffer.size(), available);
    std::copy_n(input_.begin() + static_cast<std::ptrdiff_t>(offset_), count,
                buffer.begin());
    offset_ += count;
    if (mode_ == ResponseMode::OverreportedHeaderRead && offset_ == 5) {
      return rs::core::transport::IOResult{count + 1, false};
    }
    return rs::core::transport::IOResult{count, false};
  }

  void close() noexcept override { ++close_count_; }

 private:
  void append_message(char tag, const char* payload, std::size_t size) {
    input_.push_back(static_cast<std::byte>(tag));
    const auto length = static_cast<std::uint32_t>(size + 4);
    input_.push_back(static_cast<std::byte>((length >> 24) & 0xff));
    input_.push_back(static_cast<std::byte>((length >> 16) & 0xff));
    input_.push_back(static_cast<std::byte>((length >> 8) & 0xff));
    input_.push_back(static_cast<std::byte>(length & 0xff));
    const auto* bytes = reinterpret_cast<const std::byte*>(payload);
    input_.insert(input_.end(), bytes, bytes + size);
  }

  std::vector<std::byte> input_;
  ResponseMode mode_;
  std::size_t offset_{0};
  std::size_t connect_count_{0};
  std::size_t send_count_{0};
  std::size_t close_count_{0};
  bool zero_returned_{false};
};

// Exercise the factory with a real parser and deterministic wire responses.
TEST(DatabaseFactoryTest, SelectedBackendUsesConfiguredTransportForStartupAndQuery) {
  using rs::core::database::DatabaseFactory;
  for (const bool explicit_type : {false, true}) {
    SCOPED_TRACE(explicit_type);
    auto transport = std::make_unique<ScriptedBackendTransport>(
        ScriptedBackendTransport::ResponseMode::QueryParameterStatus);
    auto* observed = transport.get();
    auto connection = explicit_type
        ? DatabaseFactory::create_connection(
              DatabaseFactory::get_compiled_database_type(), std::move(transport))
        : DatabaseFactory::create_connection(std::move(transport));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    EXPECT_TRUE(connection->server_version().empty());
    ASSERT_TRUE(connection->connect(settings).has_value());
    EXPECT_TRUE(connection->is_connected());
    EXPECT_EQ("17.6", connection->server_version());
    EXPECT_EQ(1u, observed->connect_count());
    EXPECT_EQ(1u, observed->send_count());
    ASSERT_TRUE(connection->execute_query(
        "SET application_name = 'changed'",
        rs::util::make_deadline(std::chrono::seconds(1))).has_value());
    EXPECT_EQ("17.6", connection->server_version());
    const auto owned_version = connection->server_version();
    EXPECT_EQ(2u, observed->send_count());
    connection->disconnect();
    EXPECT_FALSE(connection->is_connected());
    EXPECT_TRUE(connection->server_version().empty());
    EXPECT_EQ("17.6", owned_version);
    EXPECT_EQ(1u, observed->close_count());
  }
}

TEST(DatabaseFactoryTest, SelectedBackendPreservesAuthenticationFailureCleanup) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto& [mode, expected] : {
           std::pair{Mode::AuthRejected, rs::util::DbErrorCode::AuthenticationFailed},
           std::pair{Mode::AuthenticationTimeout, rs::util::DbErrorCode::Timeout}}) {
    SCOPED_TRACE(static_cast<int>(mode));
    auto transport = std::make_unique<ScriptedBackendTransport>(mode);
    auto* observed = transport.get();
    auto connection = rs::core::database::DatabaseFactory::create_connection(
        std::move(transport));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    const auto result = connection->connect(settings);
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(expected), result.error());
    EXPECT_FALSE(connection->is_connected());
    EXPECT_EQ(1u, observed->close_count());
  }
}

TEST(DatabaseFactoryTest,
     SelectedBackendDoesNotSendCleartextPasswordWithoutVerifiedTls) {
  auto transport = std::make_unique<ScriptedBackendTransport>(
      ScriptedBackendTransport::ResponseMode::CleartextAuthentication);
  auto* observed = transport.get();
  auto connection = rs::core::database::DatabaseFactory::create_connection(
      std::move(transport));
  rs::core::database::ConnectionSettings settings;
  settings.user = "alice";
  settings.password = "top-secret";
  settings.use_ssl = false;

  const auto result = connection->connect(settings);

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_NE(std::string::npos,
            result.error_message().find("requires verified TLS"));
  EXPECT_EQ(1u, observed->send_count());
  EXPECT_FALSE(connection->is_connected());
  EXPECT_EQ(1u, observed->close_count());
}

TEST(DatabaseFactoryTest, SelectedBackendDoesNotDowngradeRefusedTls) {
  auto transport = std::make_unique<ScriptedTlsTransport>();
  auto* observed = transport.get();
  auto connection = rs::core::database::DatabaseFactory::create_connection(
      std::move(transport));
  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = true;
  const auto result = connection->connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::TLSError),
            result.error());
  EXPECT_FALSE(connection->is_connected());
  EXPECT_EQ(1u, observed->close_count());
}

TEST(DatabaseFactoryTest, SelectedBackendAppliesCustomCaBeforeTlsNegotiation) {
  auto transport = std::make_unique<ScriptedTlsTransport>();
  auto* observed = transport.get();
  auto connection = rs::core::database::DatabaseFactory::create_connection(
      std::move(transport));
  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = true;
  settings.ssl_ca_file = "/test/private-ca.pem";

  const auto result = connection->connect(settings);

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ("/test/private-ca.pem", observed->ca_file());
  EXPECT_TRUE(observed->ca_directory().empty());
}

TEST(DatabaseFactoryTest, SelectedBackendRejectsAmbiguousOrPlaintextCaPolicy) {
  for (const bool plaintext : {false, true}) {
    auto transport = std::make_unique<ScriptedBackendTransport>(
        ScriptedBackendTransport::ResponseMode::ValidStartup);
    auto* observed = transport.get();
    auto connection = rs::core::database::DatabaseFactory::create_connection(
        std::move(transport));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = !plaintext;
    settings.ssl_ca_file = "/test/private-ca.pem";
    settings.ssl_ca_dir = plaintext ? std::string{} : "/test/certs";

    const auto result = connection->connect(settings);

    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter),
              result.error());
    EXPECT_EQ(0u, observed->send_count());
  }
}

TEST(DatabaseFactoryTest, MarkerCountingUsesBackendLexicalRulesWithoutConnecting) {
  auto connection = rs::core::database::DatabaseFactory::create_connection();
  EXPECT_FALSE(connection->is_connected());
  const auto& dialect = rs::core::database::configured_backend_provider().sql_dialect();
  EXPECT_EQ(0u, dialect.count_parameter_markers("SELECT 1"));
  EXPECT_EQ(2u, dialect.count_parameter_markers("SELECT ?, ?"));
  EXPECT_EQ(1u, dialect.count_parameter_markers(
      R"sql(SELECT '?', "?", $$?$$, $tag$?$tag$, ? /* ? /* ? */ */ -- ?
)sql"));
  EXPECT_FALSE(connection->is_connected());
}

TEST(DatabaseFactoryTest, GenericConnectionUsesInjectedParserMarkerSemantics) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<odbcpp::test::MockProtocolParser>());
  // The mock has no SQL quoting rules: this differs from PostgreSQL's count.
  EXPECT_EQ(2u, connection.count_parameter_markers("SELECT '?', ?"));
  EXPECT_EQ(0u, connection.count_parameter_markers(""));
  EXPECT_FALSE(connection.is_connected());
}

TEST(DatabaseFactoryTest, UnsupportedSelectionReleasesTransferredTransport) {
  class OwnedTransport final : public rs::core::transport::ITransport {
   public:
    explicit OwnedTransport(int& destroyed) : destroyed_(destroyed) {}
    ~OwnedTransport() override { ++destroyed_; }
    rs::util::Result<void> connect(std::string_view, uint16_t,
                                  rs::util::Deadline) override {
      ADD_FAILURE() << "Unsupported backend must not connect";
      return {};
    }
    rs::util::Result<rs::core::transport::IOResult> send(
        std::span<const std::byte>, rs::util::Deadline) override {
      ADD_FAILURE() << "Unsupported backend must not send";
      return {rs::util::DbErrorCode::NetworkError};
    }
    rs::util::Result<rs::core::transport::IOResult> recv(
        std::span<std::byte>, rs::util::Deadline) override {
      ADD_FAILURE() << "Unsupported backend must not receive";
      return {rs::util::DbErrorCode::NetworkError};
    }
    void close() noexcept override {}
   private:
    int& destroyed_;
  };
  int destroyed = 0;
  EXPECT_THROW(rs::core::database::DatabaseFactory::create_connection(
      static_cast<rs::core::database::DatabaseType>(-1),
      std::make_unique<OwnedTransport>(destroyed)), std::runtime_error);
  EXPECT_EQ(1, destroyed);
}

TEST(ConnectionLivenessTest, FailedAuthenticationClosesTransport) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto& [mode, expected_error] : {
           std::pair{Mode::AuthRejected,
                     rs::util::DbErrorCode::AuthenticationFailed},
           std::pair{Mode::AuthenticationTimeout,
                     rs::util::DbErrorCode::Timeout}}) {
    auto transport = std::make_unique<ScriptedBackendTransport>(mode);
    auto* observed_transport = transport.get();
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::move(transport));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;

    const auto result = connection.connect(settings);
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(expected_error), result.error());
    EXPECT_FALSE(connection.is_connected());
    EXPECT_EQ(1u, observed_transport->close_count());
  }
}

TEST(ConnectionLivenessTest, MalformedCredentialsFailBeforeConnectingAndAllowRetry) {
  enum class Field { User, Database, Password };
  for (const auto field : {Field::User, Field::Database, Field::Password}) {
    SCOPED_TRACE(static_cast<int>(field));
    auto transport = std::make_unique<ScriptedBackendTransport>();
    auto* observed_transport = transport.get();
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::move(transport));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    settings.user = "alice";
    settings.database = "postgres";
    settings.password = "secret";
    if (field == Field::User) {
      settings.user = std::string("alice\0admin", sizeof("alice\0admin") - 1);
    } else if (field == Field::Database) {
      settings.database = std::string(
          "postgres\0other", sizeof("postgres\0other") - 1);
    } else {
      settings.password = std::string(
          "secret\0other", sizeof("secret\0other") - 1);
    }

    rs::core::database::BackendResult<void> result;
    EXPECT_NO_THROW(result = connection.connect(settings));
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter),
              result.error());
    EXPECT_NE(std::string::npos, result.error_message().find("embedded NUL"));
    EXPECT_EQ(0u, observed_transport->connect_count());
    EXPECT_EQ(0u, observed_transport->send_count());
    EXPECT_EQ(0u, observed_transport->close_count());
    EXPECT_FALSE(connection.is_connected());

    settings.user = "alice";
    settings.database = "postgres";
    settings.password = "secret";
    ASSERT_TRUE(connection.connect(settings).has_value());
    EXPECT_TRUE(connection.is_connected());
    EXPECT_EQ(1u, observed_transport->connect_count());
    EXPECT_EQ(1u, observed_transport->send_count());
  }
}

TEST(ConnectionLivenessTest, FailedStartupWriteClosesTransport) {
  auto transport = std::make_unique<ScriptedBackendTransport>(
      ScriptedBackendTransport::ResponseMode::OverreportedStartupWrite);
  auto* observed_transport = transport.get();
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::move(transport));
  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;

  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
  EXPECT_EQ(1u, observed_transport->close_count());
}

TEST(ConnectionLivenessTest, RefusedTlsClosesTransport) {
  auto transport = std::make_unique<ScriptedTlsTransport>();
  auto* observed_transport = transport.get();
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::move(transport));
  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = true;

  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::TLSError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
  EXPECT_EQ(1u, observed_transport->close_count());
}

TEST(ConnectionLivenessTest, OverreportedSslRequestWriteIsProtocolError) {
  auto transport = std::make_unique<ScriptedTlsTransport>(
      ScriptedTlsTransport::Mode::OverreportedWrite);
  auto* observed_transport = transport.get();
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::move(transport));
  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = true;

  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
  EXPECT_EQ(1u, observed_transport->close_count());
}

TEST(ConnectionLivenessTest, InvalidSslNegotiationReadsKeepTheirErrorClass) {
  using Mode = ScriptedTlsTransport::Mode;
  for (const auto& [mode, expected_error] : {
           std::pair{Mode::NoProgress, rs::util::DbErrorCode::NetworkError},
           std::pair{Mode::Eof, rs::util::DbErrorCode::NetworkError},
           std::pair{Mode::EofWithAccept, rs::util::DbErrorCode::NetworkError},
           std::pair{Mode::InvalidReply, rs::util::DbErrorCode::ProtocolError},
           std::pair{Mode::OverreportedRead,
                     rs::util::DbErrorCode::ProtocolError}}) {
    SCOPED_TRACE(static_cast<int>(mode));
    auto transport = std::make_unique<ScriptedTlsTransport>(mode);
    auto* observed_transport = transport.get();
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::move(transport));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = true;

    const auto result = connection.connect(settings);
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(expected_error), result.error());
    EXPECT_FALSE(connection.is_connected());
    EXPECT_EQ(1u, observed_transport->close_count());
  }
}

TEST(ConnectionLivenessTest, ConnectErrorClassesSurvivePlainAndTlsPaths) {
  using Mode = ScriptedTlsTransport::Mode;
  for (const auto& [mode, expected_error] : {
           std::pair{Mode::InvalidConnect,
                     rs::util::DbErrorCode::InvalidParameter},
           std::pair{Mode::TimedOutConnect,
                     rs::util::DbErrorCode::Timeout},
           std::pair{Mode::FailedConnect,
                     rs::util::DbErrorCode::ConnectionFailed}}) {
    for (const bool use_ssl : {false, true}) {
      SCOPED_TRACE(static_cast<int>(mode));
      SCOPED_TRACE(use_ssl ? "TLS" : "plain");
      auto transport = std::make_unique<ScriptedTlsTransport>(mode);
      auto* observed_transport = transport.get();
      rs::core::database::GenericDatabaseConnection connection(
          std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
          std::move(transport));
      rs::core::database::ConnectionSettings settings;
      settings.use_ssl = use_ssl;

      const auto result = connection.connect(settings);
      ASSERT_TRUE(result.has_error());
      EXPECT_EQ(rs::util::make_error_code(expected_error), result.error());
      EXPECT_NE(std::string::npos, result.error_message().find("injected"));
      EXPECT_FALSE(connection.is_connected());
      EXPECT_EQ(1u, observed_transport->close_count());
    }
  }
}

TEST(ConnectionLivenessTest, FailedServerTripMarksConnectionDead) {
  auto transport = std::make_unique<FailingQueryTransport>();
  auto* observed_transport = transport.get();
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<odbcpp::test::MockProtocolParser>(),
      std::move(transport));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  ASSERT_TRUE(connection.is_connected());

  const auto result = connection.execute_query(
      "SELECT 1", rs::util::make_deadline(std::chrono::seconds(1)));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::NetworkError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
  EXPECT_EQ(1u, observed_transport->close_count());
}

TEST(ConnectionLivenessTest, RetainsPostgresqlStartupParameters) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<
          rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>());

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  EXPECT_EQ("17.6", connection.get_parameter("server_version"));
  EXPECT_EQ("odbcpp", connection.get_parameter("application_name"));
  EXPECT_TRUE(connection.get_parameter("missing").empty());
}

TEST(ConnectionLivenessTest, QueryParameterStatusUpdatesNegotiatedValue) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::QueryParameterStatus));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  EXPECT_EQ("odbcpp", connection.get_parameter("application_name"));
  const auto result = connection.execute_query(
      "SET application_name = 'changed'",
      rs::util::make_deadline(std::chrono::seconds(1)));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ("changed", connection.get_parameter("application_name"));
  EXPECT_TRUE(connection.is_connected());
}

TEST(ConnectionLivenessTest, MalformedQueryParameterStatusClosesConnection) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::MalformedQueryParameterStatus));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  const auto result = connection.execute_query(
      "SET application_name = 'changed'",
      rs::util::make_deadline(std::chrono::seconds(1)));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_EQ("odbcpp", connection.get_parameter("application_name"));
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, InvalidSqlDoesNotEscapeOrDisconnect) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>());

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  const std::string sql("SELECT 1\0;SELECT 2",
                        sizeof("SELECT 1\0;SELECT 2") - 1);
  const auto result = connection.execute_query(
      sql, rs::util::make_deadline(std::chrono::seconds(1)));

  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter),
            result.error());
  EXPECT_TRUE(connection.is_connected());
}

TEST(ConnectionLivenessTest, RejectsMalformedStartupParameters) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<
          rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::MalformedStartup));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, MalformedAuthIsNotReportedAsBadCredentials) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::MalformedAuth));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, OversizedControlFrameClosesConnection) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::OversizedStartupReady));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, LargeDataRowLengthsDoNotPreallocatePayload) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::TooLargeDataRow,
                          Mode::IncompleteLargeDataRow}) {
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::make_unique<ScriptedBackendTransport>(mode));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    // Exercise incremental body reads independently of the smaller startup
    // policy ceiling; the default ceiling is tested separately below.
    settings.startup_response_limits.max_wire_bytes = std::size_t{1} << 30;
    const auto result = connection.connect(settings);
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(
                  mode == Mode::TooLargeDataRow
                      ? rs::util::DbErrorCode::ProtocolError
                      : rs::util::DbErrorCode::NetworkError),
              result.error());
    EXPECT_FALSE(connection.is_connected());
  }
}

TEST(ConnectionLivenessTest, ServerAuthRejectionRemainsCredentialFailure) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::AuthRejected));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(
                rs::util::DbErrorCode::AuthenticationFailed), result.error());
  EXPECT_NE(result.error_message().find("password authentication failed"),
            std::string::npos);
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, ReadyWithoutAuthenticationOkIsProtocolError) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::ReadyWithoutAuth));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, ScramOkWithoutServerFinalIsProtocolError) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::ScramOkWithoutServerFinal));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  settings.user = "postgres";
  settings.password = "postgres";
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, ScramCannotDowngradeToCleartext) {
  auto transport = std::make_unique<ScriptedBackendTransport>(
      ScriptedBackendTransport::ResponseMode::ScramDowngradeToCleartext);
  const auto* observed_transport = transport.get();
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::move(transport));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  settings.user = "postgres";
  settings.password = "postgres";
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_NE(std::string::npos, result.error_message().find("SCRAM"));
  EXPECT_EQ(2u, observed_transport->send_count());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, ZeroProgressFrameReadsFailPromptly) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::ZeroHeaderRead, Mode::ZeroBodyRead}) {
    SCOPED_TRACE(static_cast<int>(mode));
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::make_unique<ScriptedBackendTransport>(mode));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    const auto result = connection.connect(settings);
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::NetworkError),
              result.error());
    EXPECT_NE(std::string::npos,
              result.error_message().find("made no progress"));
    EXPECT_FALSE(connection.is_connected());
  }
}

TEST(ConnectionLivenessTest, OverreportedTransportCountsAreProtocolErrors) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {
           Mode::OverreportedHeaderRead, Mode::OverreportedStartupWrite}) {
    SCOPED_TRACE(static_cast<int>(mode));
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::make_unique<ScriptedBackendTransport>(mode));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    const auto result = connection.connect(settings);
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
              result.error());
    EXPECT_NE(std::string::npos,
              result.error_message().find("exceeded requested"));
    EXPECT_FALSE(connection.is_connected());
  }
}

TEST(ConnectionLivenessTest, OutOfPhaseStartupMessagesAreProtocolErrors) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {
           Mode::DuplicateAuthenticationOk,
           Mode::ChallengeAfterAuthenticationOk,
           Mode::ParameterStatusBeforeAuthenticationOk,
           Mode::BackendKeyBeforeAuthenticationOk,
           Mode::UnexpectedQueryFrameBeforeAuthenticationOk,
           Mode::UnexpectedQueryFrameAfterAuthenticationOk}) {
    SCOPED_TRACE(static_cast<int>(mode));
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::make_unique<ScriptedBackendTransport>(mode));

    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    const auto result = connection.connect(settings);
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
              result.error());
    EXPECT_FALSE(connection.is_connected());
  }
}

TEST(ConnectionLivenessTest, NoticeAfterAuthenticationOkAllowsStartup) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::NoticeAfterAuthenticationOk));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  EXPECT_TRUE(connection.is_connected());
  EXPECT_EQ("17.6", connection.get_parameter("server_version"));
}

TEST(ConnectionLivenessTest, MalformedNoticeCannotCompleteStartup) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::MalformedNoticeMissingField,
                          Mode::MalformedNoticeDuplicateField,
                          Mode::MalformedNoticeUnterminated}) {
    SCOPED_TRACE(static_cast<int>(mode));
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::make_unique<ScriptedBackendTransport>(mode));

    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    const auto result = connection.connect(settings);
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
              result.error());
    EXPECT_FALSE(connection.is_connected());
  }
}

TEST(ConnectionLivenessTest, ErrorAfterAuthenticationIsStartupFailure) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::StartupRejectedAfterAuth));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ConnectionFailed),
            result.error());
  EXPECT_NE(result.error_message().find("startup option rejected"),
            std::string::npos);
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, AuthenticationSqlstateAfterOkIsCredentialFailure) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::LoginRejectedAfterAuth));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(
                rs::util::DbErrorCode::AuthenticationFailed), result.error());
  EXPECT_NE(result.error_message().find("role does not exist"),
            std::string::npos);
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, MalformedStartupReadyCannotOpenConnection) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::MalformedStartupReady));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, MalformedBackendKeyCannotOpenConnection) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::MalformedBackendKey));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = connection.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, MalformedQueryReadyClosesLogicalConnection) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::MalformedQueryReady));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  const auto result = connection.execute_query(
      "SELECT 1", rs::util::make_deadline(std::chrono::seconds(1)));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, StartupOnlyFramesCannotAppearDuringQuery) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {
           Mode::AuthenticationDuringQuery, Mode::BackendKeyDuringQuery}) {
    SCOPED_TRACE(static_cast<int>(mode));
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::make_unique<ScriptedBackendTransport>(mode));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    ASSERT_TRUE(connection.connect(settings).has_value());
    const auto result = connection.execute_query(
        "SELECT 1", rs::util::make_deadline(std::chrono::seconds(1)));
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
              result.error());
    EXPECT_FALSE(connection.is_connected());
  }
}

TEST(ConnectionLivenessTest, MismatchedDataRowClosesLogicalConnection) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::MismatchedDataRow));
  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  const auto result = connection.execute_query(
      "SELECT 1", rs::util::make_deadline(std::chrono::seconds(1)));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, UnannouncedDataRowClosesConnection) {
  auto transport = std::make_unique<ScriptedBackendTransport>(
      ScriptedBackendTransport::ResponseMode::UnannouncedDataRow);
  auto* observed_transport = transport.get();
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::move(transport));
  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());

  const auto result = connection.execute_query(
      "SELECT 1", rs::util::make_deadline(std::chrono::seconds(1)));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
  EXPECT_EQ(1u, observed_transport->close_count());
}

TEST(ConnectionLivenessTest, DuplicateRowDescriptionClosesConnection) {
  auto transport = std::make_unique<ScriptedBackendTransport>(
      ScriptedBackendTransport::ResponseMode::DuplicateRowDescription);
  auto* observed_transport = transport.get();
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::move(transport));
  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());

  const auto result = connection.execute_query(
      "SELECT 0", rs::util::make_deadline(std::chrono::seconds(1)));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
  EXPECT_EQ(1u, observed_transport->close_count());
}

TEST(ConnectionLivenessTest, UnknownQueryFrameClosesConnection) {
  auto transport = std::make_unique<ScriptedBackendTransport>(
      ScriptedBackendTransport::ResponseMode::UnknownQueryFrame);
  auto* observed_transport = transport.get();
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::move(transport));
  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());

  const auto result = connection.execute_query(
      "SELECT 0", rs::util::make_deadline(std::chrono::seconds(1)));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
  EXPECT_EQ(1u, observed_transport->close_count());
}

TEST(ConnectionLivenessTest, ExtendedFrameDuringSimpleQueryClosesConnection) {
  for (const char tag : {'1', '2', 't', 'n'}) {
    SCOPED_TRACE(tag);
    auto transport = std::make_unique<ScriptedBackendTransport>(
        ScriptedBackendTransport::ResponseMode::ExtendedFrameDuringSimpleQuery,
        tag);
    auto* observed_transport = transport.get();
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::move(transport));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    ASSERT_TRUE(connection.connect(settings).has_value());

    const auto result = connection.execute_query(
        "SELECT 0", rs::util::make_deadline(std::chrono::seconds(1)));
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
              result.error());
    EXPECT_FALSE(connection.is_connected());
    EXPECT_EQ(1u, observed_transport->close_count());
  }
}

TEST(ConnectionLivenessTest, ResultAfterErrorClosesConnection) {
  auto transport = std::make_unique<ScriptedBackendTransport>(
      ScriptedBackendTransport::ResponseMode::ResultAfterError);
  auto* observed_transport = transport.get();
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::move(transport));
  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());

  const auto result = connection.execute_query(
      "SELECT 1; SELECT 1 / 0; SELECT 2",
      rs::util::make_deadline(std::chrono::seconds(1)));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
  EXPECT_EQ(1u, observed_transport->close_count());
}

TEST(ConnectionLivenessTest, NotificationPayloadMustBeComplete) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::ValidNotificationDuringQuery,
                          Mode::MalformedNotificationDuringQuery}) {
    SCOPED_TRACE(static_cast<int>(mode));
    auto transport = std::make_unique<ScriptedBackendTransport>(mode);
    auto* observed_transport = transport.get();
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::move(transport));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    ASSERT_TRUE(connection.connect(settings).has_value());

    const auto result = connection.execute_query(
        "SELECT 0", rs::util::make_deadline(std::chrono::seconds(1)));
    if (mode == Mode::ValidNotificationDuringQuery) {
      ASSERT_TRUE(result.has_value()) << result.error_message();
      EXPECT_TRUE(connection.is_connected());
    } else {
      ASSERT_TRUE(result.has_error());
      EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
                result.error());
      EXPECT_FALSE(connection.is_connected());
      EXPECT_EQ(1u, observed_transport->close_count());
    }
  }
}

TEST(ConnectionLivenessTest, MalformedEmptyQueryResponseClosesConnection) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::MalformedEmptyQueryResponse));
  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  const auto result = connection.execute_query(
      "", rs::util::make_deadline(std::chrono::seconds(1)));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, CopyStreamingFailsPromptlyAsUnsupported) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {
           Mode::CopyInDuringQuery, Mode::CopyOutDuringQuery,
           Mode::CopyBothDuringQuery}) {
    SCOPED_TRACE(static_cast<int>(mode));
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::make_unique<ScriptedBackendTransport>(mode));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    ASSERT_TRUE(connection.connect(settings).has_value());
    const auto result = connection.execute_query(
        "COPY sample TO STDOUT", rs::util::make_deadline(
                                    std::chrono::seconds(1)));
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::UnsupportedFeature),
              result.error());
    EXPECT_FALSE(connection.is_connected());
  }
}

TEST(ConnectionLivenessTest, UnsolicitedCopyFramesFailBeforePayloadRead) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {
           Mode::UnsolicitedCopyData, Mode::UnsolicitedCopyDone,
           Mode::OversizedUnsolicitedCopyData}) {
    SCOPED_TRACE(static_cast<int>(mode));
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::make_unique<ScriptedBackendTransport>(mode));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    ASSERT_TRUE(connection.connect(settings).has_value());
    const auto result = connection.execute_query(
        "SELECT 1", rs::util::make_deadline(std::chrono::seconds(1)));
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
              result.error());
    EXPECT_FALSE(connection.is_connected());
  }
}

TEST(ConnectionLivenessTest, BinaryResultRowsAreNotExposedAsText) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::BinaryResultRow,
                          Mode::BinaryAdditionalResultRow}) {
    SCOPED_TRACE(static_cast<int>(mode));
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::make_unique<ScriptedBackendTransport>(mode));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    ASSERT_TRUE(connection.connect(settings).has_value());
    const auto result = connection.execute_query(
        "SELECT 42", rs::util::make_deadline(std::chrono::seconds(1)));
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::UnsupportedFeature),
              result.error());
    EXPECT_TRUE(connection.is_connected());
  }
}

TEST(ConnectionLivenessTest, QueryNeedsACompletionBeforeReady) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::ReadyOnlyQuery,
                          Mode::RowsWithoutCompletion}) {
    SCOPED_TRACE(static_cast<int>(mode));
    for (const bool prepared : {false, true}) {
      SCOPED_TRACE(prepared);
      rs::core::database::GenericDatabaseConnection connection(
          std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
          std::make_unique<ScriptedBackendTransport>(mode));
      rs::core::database::ConnectionSettings settings;
      settings.use_ssl = false;
      ASSERT_TRUE(connection.connect(settings).has_value());
      const auto deadline = rs::util::make_deadline(std::chrono::seconds(1));
      const auto result = prepared
          ? connection.execute_prepared(
                "SELECT 7",
                std::span<const rs::core::database::QueryParameter>{}, deadline)
          : connection.execute_query("SELECT 7", deadline);
      ASSERT_TRUE(result.has_error());
      EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
                result.error());
      EXPECT_FALSE(connection.is_connected());
    }
  }
}

TEST(ConnectionLivenessTest, EmptyQueryAndDescribeRemainValid) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::EmptyQueryResponse,
                          Mode::DescriptionNoData}) {
    SCOPED_TRACE(static_cast<int>(mode));
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::make_unique<ScriptedBackendTransport>(mode));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    ASSERT_TRUE(connection.connect(settings).has_value());
    const auto deadline = rs::util::make_deadline(std::chrono::seconds(1));
    const auto result = mode == Mode::EmptyQueryResponse
        ? connection.execute_query("", deadline)
        : connection.describe_statement("UPDATE sample SET value = 1", {},
                                        deadline);
    ASSERT_TRUE(result.has_value()) << result.error_message();
    EXPECT_TRUE(connection.is_connected());
  }
}

TEST(ConnectionLivenessTest, DescriptionRequiresCompleteMetadataSequence) {
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {
           Mode::ReadyOnlyQuery, Mode::DescriptionMissingParse,
           Mode::DescriptionMissingParameters,
           Mode::DescriptionMissingResult,
           Mode::DescriptionOutOfOrder}) {
    SCOPED_TRACE(static_cast<int>(mode));
    rs::core::database::GenericDatabaseConnection connection(
        std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
        std::make_unique<ScriptedBackendTransport>(mode));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    ASSERT_TRUE(connection.connect(settings).has_value());
    const auto result = connection.describe_statement(
        "UPDATE sample SET value = 1", {},
        rs::util::make_deadline(std::chrono::seconds(1)));
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
              result.error());
    EXPECT_FALSE(connection.is_connected());
  }
}

TEST(ConnectionLivenessTest, DescriptionServerErrorRemainsQueryFailure) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::DescriptionServerError));
  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  const auto result = connection.describe_statement(
      "invalid SQL", {}, rs::util::make_deadline(std::chrono::seconds(1)));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed),
            result.error());
  EXPECT_EQ(result.backend_error().native_state, "42601");
  EXPECT_TRUE(connection.is_connected());
}

TEST(ConnectionLivenessTest, MalformedErrorCannotBecomeSuccessfulQuery) {
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::make_unique<ScriptedBackendTransport>(
          ScriptedBackendTransport::ResponseMode::MalformedQueryError));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(connection.connect(settings).has_value());
  const auto result = connection.execute_query(
      "SELECT 1", rs::util::make_deadline(std::chrono::seconds(1)));
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
}

TEST(ConnectionLivenessTest, MalformedQueryResultClosesLogicalConnection) {
  auto transport = std::make_unique<ScriptedBackendTransport>(
      ScriptedBackendTransport::ResponseMode::MalformedQuery);
  auto* observed_transport = transport.get();
  rs::core::database::GenericDatabaseConnection connection(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
      std::move(transport));

  rs::core::database::ConnectionSettings settings;
  settings.use_ssl = false;
  const auto connected = connection.connect(settings);
  ASSERT_TRUE(connected.has_value()) << connected.error_message();
  ASSERT_TRUE(connection.is_connected());

  const auto deadline = rs::util::make_deadline(std::chrono::seconds(1));
  const auto result = connection.execute_query("SELECT 1", deadline);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::ProtocolError),
            result.error());
  EXPECT_FALSE(connection.is_connected());
  EXPECT_EQ(1u, observed_transport->close_count());

  const auto retry = connection.execute_query("SELECT 2", deadline);
  ASSERT_TRUE(retry.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::NotConnected),
            retry.error());
  EXPECT_EQ(1u, observed_transport->close_count());
}

}  // namespace

TEST(DatabaseDialectTest, SelectedBackendTranslatesAndRecoversAfterErrorsWithoutIo) {
  using rs::core::database::SqlTranslationError;
  auto backend = rs::core::database::DatabaseFactory::create_connection();
  const auto& dialect = rs::core::database::configured_backend_provider().sql_dialect();
  const auto good = dialect.translate_sql("SELECT {fn UCASE('ok')}");
  ASSERT_TRUE(good);
  EXPECT_EQ("SELECT UPPER('ok')", good.sql);
  for (const auto& [sql, expected] : {
      std::pair{"SELECT {d '2023-02-29'}", SqlTranslationError::InvalidDatetime},
      std::pair{"SELECT {fn UCASE('ok')", SqlTranslationError::InvalidSyntax},
      std::pair{"{?= call answer()}", SqlTranslationError::Unsupported}}) {
    const auto bad = dialect.translate_sql(sql);
    EXPECT_FALSE(bad);
    EXPECT_EQ(expected, bad.error);
    EXPECT_FALSE(bad.message.empty());
    EXPECT_TRUE(dialect.translate_sql("SELECT {d '2024-02-29'}"));
  }
  EXPECT_FALSE(backend->is_connected());
}

TEST(DatabaseDialectTest, GenericConnectionHonorsDifferentParserDialect) {
  rs::core::database::GenericDatabaseConnection backend(
      std::make_unique<odbcpp::test::MockProtocolParser>());
  for (const auto* sql : {"SELECT {fn UCASE('ok')}", "{?= call answer()}"}) {
    // This parser passes its dialect through, unlike the PostgreSQL backend.
    const auto translated = backend.translate_sql(sql);
    ASSERT_TRUE(translated);
    EXPECT_EQ(sql, translated.sql);
  }
  EXPECT_FALSE(backend.is_connected());
}

namespace {
class TransactionProbe final : public rs::core::database::postgres::PgDatabaseConnection {
public:
  TransactionProbe() : PgDatabaseConnection() {}
  std::string command;
  rs::util::Deadline observed_deadline{};
  std::optional<rs::util::DbErrorCode> failure;
  int calls = 0;
  rs::core::database::SessionSnapshot snapshot;
  rs::core::database::BackendResult<rs::core::database::QueryResult> execute_query(
      std::string_view sql, rs::util::Deadline deadline) override {
    ++calls;
    command = sql;
    observed_deadline = deadline;
    if (failure) {
      rs::core::database::BackendError error{rs::util::make_error_code(*failure), "injected transaction failure"};
      if (*failure == rs::util::DbErrorCode::QueryFailed) {
        error.native_state = "40P01";
        error.native_code = 1234;  // Synthetic backend detail, not a PostgreSQL code.
        error.session_state = rs::core::database::SessionState::FailedTransaction;
        error.disposition = rs::core::database::SessionDisposition::ResetRequired;
      } else {
        error.session_state = rs::core::database::SessionState::Disconnected;
      }
      return error;
    }
    return {rs::core::database::QueryResult{}, snapshot};
  }
};
}

TEST(BackendTransactionTest, CommandsPreserveCallerDeadline) {
  using rs::core::database::TransactionAction;
  TransactionProbe backend;
  const auto deadline = rs::util::make_deadline(std::chrono::seconds(5));
  for (const auto action : {TransactionAction::Begin, TransactionAction::Commit,
                            TransactionAction::Rollback}) {
    const auto result = backend.transaction(action, deadline);
    ASSERT_FALSE(result.has_error());
    EXPECT_EQ(action == TransactionAction::Begin ? "BEGIN"
        : action == TransactionAction::Commit ? "COMMIT" : "ROLLBACK", backend.command);
    EXPECT_EQ(deadline, backend.observed_deadline);
  }
  EXPECT_EQ(3, backend.calls);
}

TEST(BackendTransactionTest, AdvertisedIsolationLevelsHaveCommands) {
  using namespace rs::core::database;
  TransactionProbe backend;
  const auto capabilities = backend.transaction_capabilities();
  EXPECT_TRUE(capabilities.supported);
  EXPECT_TRUE(capabilities.transactional_ddl);
  EXPECT_EQ(TransactionIsolation::ReadCommitted, capabilities.default_isolation);
  const auto deadline = rs::util::Deadline::min();
  const std::array names{"READ UNCOMMITTED", "READ COMMITTED", "REPEATABLE READ", "SERIALIZABLE"};
  for (std::size_t i = 0; i < transaction_isolations.size(); ++i) {
    EXPECT_TRUE(capabilities.supports(transaction_isolations[i]));
    ASSERT_FALSE(backend.set_transaction_isolation(transaction_isolations[i], deadline).has_error());
    EXPECT_EQ(std::string("SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL ") + names[i], backend.command);
    EXPECT_EQ(deadline, backend.observed_deadline);
  }
}

TEST(BackendTransactionTest, RejectsInvalidEnumsWithoutIo) {
  using namespace rs::core::database;
  TransactionProbe backend;
  auto result = backend.transaction(static_cast<TransactionAction>(99), rs::util::Deadline::min());
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter), result.error());
  result = backend.set_transaction_isolation(static_cast<TransactionIsolation>(99), rs::util::Deadline::min());
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(rs::util::make_error_code(rs::util::DbErrorCode::InvalidParameter), result.error());
  EXPECT_FALSE(backend.transaction_capabilities().supports(static_cast<TransactionIsolation>(99)));
  EXPECT_EQ(0, backend.calls);
}

TEST(BackendTransactionTest, PropagatesErrorsAndAllowsSuccessfulRetry) {
  using namespace rs::core::database;
  TransactionProbe backend;
  const auto deadline = rs::util::Deadline::max();
  for (const auto error : {rs::util::DbErrorCode::Timeout, rs::util::DbErrorCode::NetworkError,
                           rs::util::DbErrorCode::QueryFailed}) {
    backend.failure = error;
    for (const bool isolation : {false, true}) {
      const auto result = isolation
          ? backend.set_transaction_isolation(TransactionIsolation::Serializable, deadline)
          : backend.transaction(TransactionAction::Commit, deadline);
      ASSERT_TRUE(result.has_error());
      EXPECT_EQ(rs::util::make_error_code(error), result.error());
      EXPECT_EQ("injected transaction failure", result.error_message());
      EXPECT_EQ(deadline, backend.observed_deadline);
    }
  }
  backend.failure.reset();
  EXPECT_FALSE(backend.transaction(TransactionAction::Rollback, deadline).has_error());
  EXPECT_FALSE(backend.set_transaction_isolation(TransactionIsolation::ReadCommitted, deadline).has_error());
}

TEST(BackendTransactionTest, GenericBackendDoesNotAssumeTransactionSupport) {
  using namespace rs::core::database;
  GenericDatabaseConnection backend(std::make_unique<odbcpp::test::MockProtocolParser>());
  EXPECT_EQ(nullptr, backend.transaction_session());
  EXPECT_FALSE(backend.is_connected());
}

TEST(BackendResultContractTest, RowsMetadataAndDeferredErrorsOutliveConnection) {
  rs::core::database::QueryResult retained;
  {
    rs::core::database::postgres::PgDatabaseConnection backend(std::make_unique<ScriptedBackendTransport>(
            ScriptedBackendTransport::ResponseMode::OwnedResultCells));
    rs::core::database::ConnectionSettings settings;
    settings.use_ssl = false;
    ASSERT_TRUE(backend.connect(settings).has_value());
    auto result = backend.execute_query("scripted result", rs::util::Deadline::max());
    ASSERT_TRUE(result.has_value()) << result.error_message();
    EXPECT_TRUE(backend.is_connected());
    retained = std::move(*result);
    backend.disconnect();
  }
  ASSERT_EQ(1u, retained.columns.size());
  EXPECT_EQ("value", retained.columns[0].name);
  ASSERT_TRUE(retained.columns[0].normalized_type);
  EXPECT_EQ(rs::core::database::ScalarType::VarChar, retained.columns[0].normalized_type->type);
  ASSERT_EQ(3u, retained.rows.size());
  for (const auto& row : retained.rows) ASSERT_EQ(1u, row.size());
  EXPECT_FALSE(retained.rows[0][0].has_value());
  ASSERT_TRUE(retained.rows[1][0].has_value());
  EXPECT_TRUE(retained.rows[1][0]->empty());
  EXPECT_EQ(std::optional<std::string>("abc"), retained.rows[2][0]);
  ASSERT_EQ(1u, retained.additional_results.size());
  ASSERT_TRUE(retained.additional_results[0].error);
  EXPECT_EQ("22012", retained.additional_results[0].error->native_state);
  EXPECT_NE(std::string::npos,
            retained.additional_results[0].error->message.find("division by zero"));
}

TEST(ConnectionLivenessTest, OwningErrorsPreserveNativeDetailAndProtocolDisposition) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::OwnedErrorIdle, Mode::OwnedErrorTransaction, Mode::OwnedErrorAborted}) {
    for (const auto operation : {BackendOperation::ExecuteDirect, BackendOperation::ExecutePrepared, BackendOperation::Describe}) {
      SCOPED_TRACE(static_cast<int>(mode));
      SCOPED_TRACE(static_cast<int>(operation));
      BackendResult<QueryResult> saved(QueryResult{});
      {
        GenericDatabaseConnection connection(std::make_unique<postgres::PgProtocolParser>(),
            std::make_unique<ScriptedBackendTransport>(mode));
        EXPECT_EQ(connection.session_state(), SessionState::Disconnected);
        ConnectionSettings settings;
        settings.use_ssl = false;
        ASSERT_TRUE(connection.connect(settings));
        EXPECT_EQ(connection.session_state(), SessionState::Idle);
        const auto deadline = rs::util::make_deadline(std::chrono::seconds(1));
        auto result = operation == BackendOperation::ExecuteDirect ? connection.execute_query("broken", deadline) :
                      operation == BackendOperation::ExecutePrepared ? connection.execute_prepared("broken", std::span<const QueryParameter>{}, deadline) :
                      connection.describe_statement("broken", {}, deadline);
        ASSERT_TRUE(result.has_error());
        const auto& error = result.backend_error();
        EXPECT_EQ(error.error_class, BackendErrorClass::Server);
        EXPECT_EQ(error.native_state, "42601");
        EXPECT_EQ(error.operation, operation);
        EXPECT_FALSE(error.native_code);
        EXPECT_FALSE(error.retry_safe);
        const auto state = mode == Mode::OwnedErrorIdle ? SessionState::Idle :
                           mode == Mode::OwnedErrorTransaction ? SessionState::Transaction : SessionState::FailedTransaction;
        EXPECT_EQ(error.session_state, state);
        EXPECT_EQ(connection.session_state(), state);
        EXPECT_EQ(error.disposition, mode == Mode::OwnedErrorIdle ? SessionDisposition::Reusable : SessionDisposition::ResetRequired);
        saved = result;
        auto moved = std::move(result);
        ASSERT_TRUE(connection.execute_query("ROLLBACK", deadline));
        EXPECT_EQ(connection.session_state(), SessionState::Idle);
        EXPECT_EQ(moved.backend_error().native_state, "42601");
        EXPECT_EQ(moved.backend_error().session_state, state);
        connection.disconnect();
        EXPECT_EQ(connection.session_state(), SessionState::Disconnected);
      }
      EXPECT_EQ(saved.backend_error().native_state, "42601");
      EXPECT_EQ(saved.backend_error().message, "Query error: syntax error");
      EXPECT_EQ(saved.backend_error().operation, operation);
    }
  }
}

TEST(ConnectionLivenessTest, AmbiguousQueryFailuresRetireAndNeverRetainServerState) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::QueryReadTimeout, Mode::PartialQueryWrite, Mode::MalformedQueryReady}) {
    SCOPED_TRACE(static_cast<int>(mode));
    auto transport = std::make_unique<ScriptedBackendTransport>(mode);
    auto* observed = transport.get();
    GenericDatabaseConnection connection(std::make_unique<postgres::PgProtocolParser>(), std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    ASSERT_TRUE(connection.connect(settings));
    const auto result = connection.execute_query("SELECT 1", rs::util::make_deadline(std::chrono::seconds(1)));
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
    EXPECT_EQ(result.backend_error().session_state, SessionState::Disconnected);
    EXPECT_EQ(result.backend_error().operation, BackendOperation::ExecuteDirect);
    EXPECT_FALSE(result.backend_error().native_state);
    EXPECT_FALSE(connection.is_connected());
    EXPECT_EQ(observed->close_count(), 1U);
    const auto sends = observed->send_count();
    const auto again = connection.execute_query("SELECT 2", rs::util::make_deadline(std::chrono::seconds(1)));
    ASSERT_TRUE(again.has_error());
    EXPECT_EQ(again.backend_error().error_class, BackendErrorClass::NotConnected);
    EXPECT_EQ(again.backend_error().disposition, SessionDisposition::Retire);
    EXPECT_EQ(observed->send_count(), sends);
  }
}

TEST(BackendTransactionTest, OwningFailuresKeepContextAndDetailsAcrossBackendDestruction) {
  using namespace rs::core::database;
  BackendResult<void> saved;
  {
    TransactionProbe backend;
    backend.failure = rs::util::DbErrorCode::QueryFailed;
    for (const auto action : {TransactionAction::Begin, TransactionAction::Commit, TransactionAction::Rollback}) {
      auto result = backend.transaction(action, rs::util::Deadline::max());
      ASSERT_TRUE(result.has_error());
      EXPECT_EQ(result.backend_error().operation, action == TransactionAction::Begin ? BackendOperation::BeginTransaction :
          action == TransactionAction::Commit ? BackendOperation::CommitTransaction : BackendOperation::RollbackTransaction);
      EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::Server);
      EXPECT_EQ(result.backend_error().native_state, "40P01");
      EXPECT_EQ(result.backend_error().native_code, 1234);
      EXPECT_EQ(result.backend_error().session_state, SessionState::FailedTransaction);
      EXPECT_EQ(result.backend_error().disposition, SessionDisposition::ResetRequired);
      EXPECT_FALSE(result.backend_error().retry_safe);
      saved = result;
      auto moved = std::move(result);
      EXPECT_EQ(moved.backend_error().native_state, "40P01");
    }
    saved = backend.set_transaction_isolation(TransactionIsolation::Serializable, rs::util::Deadline::max());
    backend.failure.reset();
    EXPECT_TRUE(backend.transaction(TransactionAction::Rollback, rs::util::Deadline::max()));
  }
  ASSERT_TRUE(saved.has_error());
  EXPECT_EQ(saved.backend_error().operation, BackendOperation::SetTransactionIsolation);
  EXPECT_EQ(saved.backend_error().native_state, "40P01");
  EXPECT_EQ(saved.backend_error().native_code, 1234);
  EXPECT_EQ(saved.error_message(), "injected transaction failure");
  saved = BackendResult<void>{};
  EXPECT_TRUE(saved.has_value());
}

TEST(BackendTransactionTest, RealProtocolStateSurvivesTransactionAndIsolationAdapters) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::OwnedErrorIdle, Mode::OwnedErrorTransaction, Mode::OwnedErrorAborted}) {
    for (const auto operation : {BackendOperation::BeginTransaction, BackendOperation::CommitTransaction,
         BackendOperation::RollbackTransaction, BackendOperation::SetTransactionIsolation}) {
      SCOPED_TRACE(static_cast<int>(mode));
      SCOPED_TRACE(static_cast<int>(operation));
      auto transport = std::make_unique<ScriptedBackendTransport>(mode);
      auto* observed = transport.get();
      postgres::PgDatabaseConnection backend(std::move(transport));
      ConnectionSettings settings;
      settings.use_ssl = false;
      ASSERT_TRUE(backend.connect(settings));
      const auto deadline = rs::util::make_deadline(std::chrono::seconds(1));
      auto result = operation == BackendOperation::SetTransactionIsolation ? backend.set_transaction_isolation(TransactionIsolation::Serializable, deadline) :
          backend.transaction(operation == BackendOperation::BeginTransaction ? TransactionAction::Begin :
              operation == BackendOperation::CommitTransaction ? TransactionAction::Commit : TransactionAction::Rollback, deadline);
      ASSERT_TRUE(result.has_error());
      EXPECT_EQ(result.backend_error().operation, operation);
      EXPECT_EQ(result.backend_error().native_state, "42601");
      EXPECT_EQ(result.backend_error().disposition, mode == Mode::OwnedErrorIdle ? SessionDisposition::Reusable : SessionDisposition::ResetRequired);
      const auto state = backend.session_state();
      const auto sends = observed->send_count();
      auto invalid = backend.transaction(static_cast<TransactionAction>(99), deadline);
      ASSERT_TRUE(invalid.has_error());
      EXPECT_EQ(invalid.backend_error().operation, BackendOperation::Transaction);
      EXPECT_EQ(invalid.backend_error().error_class, BackendErrorClass::InvalidInput);
      EXPECT_EQ(invalid.backend_error().session_state, state);
      EXPECT_FALSE(invalid.backend_error().native_state);
      invalid = backend.set_transaction_isolation(static_cast<TransactionIsolation>(99), deadline);
      EXPECT_EQ(invalid.backend_error().operation, BackendOperation::SetTransactionIsolation);
      EXPECT_EQ(invalid.backend_error().session_state, state);
      EXPECT_EQ(observed->send_count(), sends);
      ASSERT_TRUE(backend.transaction(TransactionAction::Rollback, deadline));
      EXPECT_EQ(backend.session_state(), SessionState::Idle);
      EXPECT_EQ(result.backend_error().session_state, state);
      EXPECT_EQ(result.backend_error().native_state, "42601");
    }
  }
}

TEST(BackendTransactionTest, AmbiguousFailuresRetainRetirementAndOperationContext) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::QueryReadTimeout, Mode::PartialQueryWrite}) {
    for (const bool isolation : {false, true}) {
      auto transport = std::make_unique<ScriptedBackendTransport>(mode);
      auto* observed = transport.get();
      postgres::PgDatabaseConnection backend(std::move(transport));
      ConnectionSettings settings;
      settings.use_ssl = false;
      ASSERT_TRUE(backend.connect(settings));
      const auto deadline = rs::util::make_deadline(std::chrono::seconds(1));
      auto result = isolation ? backend.set_transaction_isolation(TransactionIsolation::Serializable, deadline) : backend.transaction(TransactionAction::Commit, deadline);
      ASSERT_TRUE(result.has_error());
      EXPECT_EQ(result.backend_error().operation, isolation ? BackendOperation::SetTransactionIsolation : BackendOperation::CommitTransaction);
      EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
      EXPECT_EQ(result.backend_error().session_state, SessionState::Disconnected);
      EXPECT_FALSE(result.backend_error().native_state);
      EXPECT_FALSE(result.backend_error().retry_safe);
      EXPECT_EQ(observed->close_count(), 1U);
      EXPECT_FALSE(backend.is_connected());
    }
  }
}

TEST(ConnectionLivenessTest, SetupErrorsOwnNativeDetailsAndLifecyclePhaseAfterCleanup) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::AuthRejected, Mode::StartupRejectedAfterAuth, Mode::LoginRejectedAfterAuth,
                         Mode::AuthenticationTimeout, Mode::MalformedAuth, Mode::MalformedStartupReady}) {
    SCOPED_TRACE(static_cast<int>(mode));
    BackendResult<void> saved;
    {
      auto transport = std::make_unique<ScriptedBackendTransport>(mode);
      auto* observed = transport.get();
      GenericDatabaseConnection backend(std::make_unique<postgres::PgProtocolParser>(), std::move(transport));
      ConnectionSettings settings;
      settings.use_ssl = false;
      auto result = backend.connect(settings);
      ASSERT_TRUE(result.has_error());
      EXPECT_EQ(observed->close_count(), 1U);
      EXPECT_FALSE(backend.is_connected());
      EXPECT_EQ(result.backend_error().session_state, SessionState::Disconnected);
      EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
      EXPECT_FALSE(result.backend_error().retry_safe);
      // MalformedStartupReady is delivered before AuthenticationOk.
      const bool startup = mode == Mode::StartupRejectedAfterAuth;
      EXPECT_EQ(result.backend_error().operation, startup ? BackendOperation::Startup : BackendOperation::Authenticate);
      if (mode == Mode::AuthRejected || mode == Mode::LoginRejectedAfterAuth) {
        EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::Authentication);
        EXPECT_EQ(result.backend_error().native_state, mode == Mode::AuthRejected ? "28P01" : "28000");
      } else if (mode == Mode::StartupRejectedAfterAuth) {
        EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::Connection);
        EXPECT_EQ(result.backend_error().native_state, "22023");
      } else {
        EXPECT_FALSE(result.backend_error().native_state);
        EXPECT_EQ(result.backend_error().error_class, mode == Mode::AuthenticationTimeout ? BackendErrorClass::Timeout : BackendErrorClass::Protocol);
      }
      saved = result;
      auto moved = std::move(result);
      EXPECT_EQ(moved.error_message(), saved.error_message());
      backend.disconnect();
    }
    ASSERT_TRUE(saved.has_error());
    EXPECT_FALSE(saved.error_message().empty());
    EXPECT_EQ(saved.backend_error().session_state, SessionState::Disconnected);
    EXPECT_EQ(saved.backend_error().disposition, SessionDisposition::Retire);
  }
}

TEST(ConnectionLivenessTest, InvalidReconnectKeepsExistingSessionAndPriorFailureSnapshot) {
  using namespace rs::core::database;
  auto transport = std::make_unique<ScriptedBackendTransport>();
  auto* observed = transport.get();
  GenericDatabaseConnection backend(std::make_unique<postgres::PgProtocolParser>(), std::move(transport));
  ConnectionSettings settings;
  settings.use_ssl = false;
  auto invalid = settings;
  invalid.password = std::string("secret\0hidden", 13);
  const auto first = backend.connect(invalid);
  ASSERT_TRUE(first.has_error());
  EXPECT_EQ(first.backend_error().operation, BackendOperation::Connect);
  EXPECT_EQ(first.backend_error().disposition, SessionDisposition::Retire);
  EXPECT_EQ(observed->connect_count(), 0U);
  EXPECT_EQ(observed->close_count(), 0U);
  EXPECT_EQ(first.error_message().find("secret"), std::string::npos);
  ASSERT_TRUE(backend.connect(settings));
  EXPECT_EQ(backend.session_state(), SessionState::Idle);
  const auto sends = observed->send_count();
  const auto connects = observed->connect_count();
  const auto closes = observed->close_count();
  const auto second = backend.connect(invalid);
  ASSERT_TRUE(second.has_error());
  EXPECT_EQ(second.backend_error().operation, BackendOperation::Connect);
  EXPECT_EQ(second.backend_error().error_class, BackendErrorClass::InvalidInput);
  EXPECT_EQ(second.backend_error().session_state, SessionState::Idle);
  EXPECT_EQ(second.backend_error().disposition, SessionDisposition::Reusable);
  EXPECT_FALSE(second.backend_error().native_state);
  EXPECT_TRUE(backend.is_connected());
  EXPECT_EQ(observed->send_count(), sends);
  EXPECT_EQ(observed->connect_count(), connects);
  EXPECT_EQ(observed->close_count(), closes);
  backend.disconnect();
  EXPECT_EQ(first.backend_error().session_state, SessionState::Disconnected);
  EXPECT_EQ(second.backend_error().session_state, SessionState::Idle);
}

TEST(NativeTypeLookupLivenessTest, PreservesOwnedServerAndAmbiguousFailureSnapshots) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  const std::uint32_t ids[]{90000};
  for (const auto mode : {Mode::OwnedErrorIdle, Mode::OwnedErrorTransaction, Mode::OwnedErrorAborted,
                         Mode::QueryReadTimeout, Mode::PartialQueryWrite, Mode::MalformedQueryReady}) {
    SCOPED_TRACE(static_cast<int>(mode));
    std::optional<BackendError> saved;
    {
      auto transport = std::make_unique<ScriptedBackendTransport>(mode);
      auto* observed = transport.get();
      postgres::PgDatabaseConnection backend(std::move(transport));
      ConnectionSettings settings;
      settings.use_ssl = false;
      ASSERT_TRUE(backend.connect(settings));
      auto result = backend.resolve_types(ids, rs::util::make_deadline(std::chrono::seconds(1)));
      ASSERT_TRUE(result.has_error());
      EXPECT_EQ(result.backend_error().operation, BackendOperation::ResolveTypes);
      EXPECT_FALSE(result.backend_error().native_code);
      EXPECT_FALSE(result.backend_error().retry_safe);
      const bool server = mode == Mode::OwnedErrorIdle || mode == Mode::OwnedErrorTransaction || mode == Mode::OwnedErrorAborted;
      if (server) {
        EXPECT_EQ(result.backend_error().native_state, "42601");
        EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::Server);
        EXPECT_EQ(result.backend_error().session_state, mode == Mode::OwnedErrorIdle ? SessionState::Idle :
            mode == Mode::OwnedErrorTransaction ? SessionState::Transaction : SessionState::FailedTransaction);
        EXPECT_EQ(result.backend_error().disposition, mode == Mode::OwnedErrorIdle ? SessionDisposition::Reusable : SessionDisposition::ResetRequired);
        saved = result.backend_error();
        EXPECT_TRUE(backend.transaction(TransactionAction::Rollback, rs::util::Deadline::max()));
        EXPECT_EQ(backend.session_state(), SessionState::Idle);
      } else {
        EXPECT_FALSE(result.backend_error().native_state);
        EXPECT_EQ(result.backend_error().session_state, SessionState::Disconnected);
        EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
        EXPECT_FALSE(backend.is_connected());
        EXPECT_EQ(observed->close_count(), 1U);
        saved = result.backend_error();
        const auto sends = observed->send_count();
        auto again = backend.resolve_types(ids, rs::util::Deadline::max());
        ASSERT_TRUE(again.has_error());
        EXPECT_EQ(again.backend_error().operation, BackendOperation::ResolveTypes);
        EXPECT_EQ(again.backend_error().error_class, BackendErrorClass::NotConnected);
        EXPECT_EQ(observed->send_count(), sends);
      }
      auto moved = std::move(result);
      EXPECT_EQ(moved.backend_error().native_state, saved->native_state);
      EXPECT_EQ(moved.error_message(), saved->message);
      backend.disconnect();
    }
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->operation, BackendOperation::ResolveTypes);
    EXPECT_FALSE(saved->message.empty());
  }
}

TEST(BackendResultContractTest, DeferredErrorsRetainOperationAndFinalStateAcrossLaterCalls) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::OwnedResultCells, Mode::OwnedResultCellsTransaction, Mode::OwnedResultCellsAborted}) {
    SCOPED_TRACE(static_cast<int>(mode));
    QueryResult saved;
    {
      postgres::PgDatabaseConnection backend(std::make_unique<ScriptedBackendTransport>(mode));
      ConnectionSettings settings;
      settings.use_ssl = false;
      ASSERT_TRUE(backend.connect(settings));
      auto result = backend.execute_query("first", rs::util::Deadline::max());
      ASSERT_TRUE(result);
      EXPECT_FALSE(result->error);
      ASSERT_EQ(1u, result->additional_results.size());
      ASSERT_TRUE(result->additional_results[0].error);
      saved = *result;
      auto moved = std::move(*result);
      const auto& error = *moved.additional_results[0].error;
      EXPECT_EQ(error.operation, BackendOperation::ExecuteDirect);
      EXPECT_EQ(error.error_class, BackendErrorClass::Server);
      EXPECT_EQ(error.native_state, "22012");
      EXPECT_FALSE(error.native_code);
      EXPECT_FALSE(error.retry_safe);
      EXPECT_EQ(error.session_state, mode == Mode::OwnedResultCells ? SessionState::Idle :
          mode == Mode::OwnedResultCellsTransaction ? SessionState::Transaction : SessionState::FailedTransaction);
      EXPECT_EQ(error.disposition, mode == Mode::OwnedResultCells ? SessionDisposition::Reusable : SessionDisposition::ResetRequired);
      EXPECT_EQ(error.safe_summary(), "Database server rejected the operation");
      EXPECT_TRUE(backend.is_connected());
      // This fixture has no second response: the later ambiguous failure must
      // retire the session without rewriting the first result snapshot.
      EXPECT_FALSE(backend.execute_query("later", rs::util::Deadline::max()));
      EXPECT_EQ(backend.session_state(), SessionState::Disconnected);
      backend.disconnect();
    }
    ASSERT_TRUE(saved.additional_results[0].error);
    EXPECT_EQ(saved.additional_results[0].error->message, "Query error: division by zero");
    EXPECT_EQ(saved.additional_results[0].error->native_state, "22012");
    EXPECT_EQ(saved.additional_results[0].error->operation, BackendOperation::ExecuteDirect);
  }
}

TEST(ResponseBudgetTest, ExactBoundariesSucceedAndOverflowRetiresWithoutPartialResults) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  std::size_t response_bytes = 0;
  {
    auto transport = std::make_unique<ScriptedBackendTransport>(Mode::OwnedResultCells);
    auto* observed = transport.get();
    postgres::PgDatabaseConnection backend(std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    ASSERT_TRUE(backend.connect(settings));
    const auto before = observed->bytes_read();
    ASSERT_TRUE(backend.execute_query("baseline", rs::util::Deadline::max()));
    response_bytes = observed->bytes_read() - before;
  }
  ASSERT_GT(response_bytes, 5u);
  for (const bool byte_limit : {false, true}) {
    for (const bool exact : {false, true}) {
      auto transport = std::make_unique<ScriptedBackendTransport>(Mode::OwnedResultCells);
      auto* observed = transport.get();
      postgres::PgDatabaseConnection backend(std::move(transport));
      ConnectionSettings settings;
      settings.use_ssl = false;
      if (byte_limit) settings.response_limits.max_wire_bytes = response_bytes - (exact ? 0 : 1);
      else settings.response_limits.max_messages = exact ? 7 : 6; // T, three D, C, E, Z
      ASSERT_TRUE(backend.connect(settings));
      const auto result = backend.execute_query("bounded", rs::util::Deadline::max());
      EXPECT_EQ(exact, result.has_value());
      if (exact) {
        ASSERT_TRUE(result);
        EXPECT_EQ(3u, result->rows.size());
        EXPECT_EQ(1u, result->additional_results.size());
        EXPECT_EQ(observed->close_count(), 0u);
      } else {
        ASSERT_TRUE(result.has_error());
        EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::ResourceLimit);
        EXPECT_EQ(result.backend_error().operation, BackendOperation::ExecuteDirect);
        EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
        EXPECT_EQ(result.backend_error().session_state, SessionState::Disconnected);
        EXPECT_FALSE(result.backend_error().native_state);
        EXPECT_FALSE(result.backend_error().retry_safe);
        EXPECT_FALSE(backend.is_connected());
        EXPECT_EQ(observed->close_count(), 1u);
        const auto sends = observed->send_count();
        EXPECT_FALSE(backend.execute_query("again", rs::util::Deadline::max()));
        EXPECT_EQ(observed->send_count(), sends);
      }
    }
  }
}

TEST(ResponseBudgetTest, RejectsDeclaredOversizeBeforePayloadAcrossAllOperations) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto operation : {BackendOperation::ExecuteDirect, BackendOperation::ExecutePrepared, BackendOperation::Describe}) {
    auto transport = std::make_unique<ScriptedBackendTransport>(
        operation == BackendOperation::Describe ? Mode::DescriptionServerError : Mode::OwnedResultCells);
    auto* observed = transport.get();
    postgres::PgDatabaseConnection backend(std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    settings.response_limits.max_wire_bytes = 5;
    ASSERT_TRUE(backend.connect(settings));
    const auto before = observed->bytes_read();
    const auto result = operation == BackendOperation::ExecuteDirect ? backend.execute_query("bounded", rs::util::Deadline::max()) :
        operation == BackendOperation::ExecutePrepared ? backend.execute_prepared("bounded", std::span<const QueryParameter>{}, rs::util::Deadline::max()) :
        backend.describe_statement("bounded", {}, rs::util::Deadline::max());
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(observed->bytes_read() - before, 5u);
    EXPECT_EQ(result.backend_error().operation, operation);
    EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::ResourceLimit);
    EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
    EXPECT_EQ(observed->close_count(), 1u);
  }
}

TEST(ResponseBudgetTest, InvalidLimitsDoNotMutateAnExistingSession) {
  using namespace rs::core::database;
  auto transport = std::make_unique<ScriptedBackendTransport>();
  auto* observed = transport.get();
  postgres::PgDatabaseConnection backend(std::move(transport));
  ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(backend.connect(settings));
  for (const bool bytes : {false, true}) {
    auto invalid = settings;
    if (bytes) invalid.response_limits.max_wire_bytes = 4;
    else invalid.response_limits.max_messages = 0;
    const auto connects = observed->connect_count();
    const auto result = backend.connect(invalid);
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::InvalidInput);
    EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Reusable);
    EXPECT_EQ(observed->connect_count(), connects);
    EXPECT_EQ(observed->close_count(), 0u);
    EXPECT_TRUE(backend.is_connected());
  }
}

TEST(StartupBudgetTest, ExactAndOverflowLimitsPreservePhaseAndCleanup) {
  using namespace rs::core::database;
  std::size_t startup_bytes = 0;
  {
    auto transport = std::make_unique<ScriptedBackendTransport>();
    auto* observed = transport.get();
    postgres::PgDatabaseConnection backend(std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    ASSERT_TRUE(backend.connect(settings));
    startup_bytes = observed->bytes_read();
  }
  ASSERT_GT(startup_bytes, 5u);
  for (const bool bytes : {false, true}) {
    for (const bool exact : {false, true}) {
      auto transport = std::make_unique<ScriptedBackendTransport>();
      auto* observed = transport.get();
      postgres::PgDatabaseConnection backend(std::move(transport));
      ConnectionSettings settings;
      settings.use_ssl = false;
      if (bytes) settings.startup_response_limits.max_wire_bytes = startup_bytes - (exact ? 0 : 1);
      else settings.startup_response_limits.max_messages = exact ? 5 : 4; // R, S, S, K, Z
      auto result = backend.connect(settings);
      EXPECT_EQ(exact, result.has_value());
      if (exact) {
        EXPECT_TRUE(backend.is_connected());
        EXPECT_EQ(backend.session_state(), SessionState::Idle);
        EXPECT_EQ(observed->close_count(), 0u);
      } else {
        ASSERT_TRUE(result.has_error());
        EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::ResourceLimit);
        EXPECT_EQ(result.backend_error().operation, BackendOperation::Startup);
        EXPECT_EQ(result.backend_error().session_state, SessionState::Disconnected);
        EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
        EXPECT_FALSE(result.backend_error().native_state);
        EXPECT_FALSE(result.backend_error().retry_safe);
        EXPECT_FALSE(backend.is_connected());
        EXPECT_EQ(observed->close_count(), 1u);
      }
    }
  }
  {
    auto transport = std::make_unique<ScriptedBackendTransport>();
    auto* observed = transport.get();
    postgres::PgDatabaseConnection backend(std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    settings.startup_response_limits.max_wire_bytes = 5;
    auto result = backend.connect(settings);
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.backend_error().operation, BackendOperation::Authenticate);
    EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::ResourceLimit);
    EXPECT_EQ(observed->bytes_read(), 5u);
    EXPECT_EQ(observed->close_count(), 1u);
  }
}

TEST(StartupBudgetTest, NoticesConsumeBudgetAndInvalidReconnectDoesNotTouchTransport) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const std::size_t messages : {5u, 6u}) {
    auto transport = std::make_unique<ScriptedBackendTransport>(Mode::NoticeAfterAuthenticationOk);
    auto* observed = transport.get();
    postgres::PgDatabaseConnection backend(std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    settings.startup_response_limits.max_messages = messages;
    auto result = backend.connect(settings);
    EXPECT_EQ(messages == 6, result.has_value());
    if (messages == 5) {
      ASSERT_TRUE(result.has_error());
      EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::ResourceLimit);
      EXPECT_EQ(result.backend_error().operation, BackendOperation::Startup);
      EXPECT_EQ(observed->close_count(), 1u);
    } else {
      for (const bool bytes : {false, true}) {
        auto invalid = settings;
        if (bytes) invalid.startup_response_limits.max_wire_bytes = 4;
        else invalid.startup_response_limits.max_messages = 0;
        const auto connects = observed->connect_count();
        const auto reads = observed->bytes_read();
        const auto failed = backend.connect(invalid);
        ASSERT_TRUE(failed.has_error());
        EXPECT_EQ(failed.backend_error().error_class, BackendErrorClass::InvalidInput);
        EXPECT_EQ(failed.backend_error().disposition, SessionDisposition::Reusable);
        EXPECT_TRUE(backend.is_connected());
        EXPECT_EQ(observed->connect_count(), connects);
        EXPECT_EQ(observed->bytes_read(), reads);
        EXPECT_EQ(observed->close_count(), 0u);
      }
    }
  }
}

TEST(StartupBudgetTest, DefaultCeilingRejectsHugeDeclaredFrameBeforePayload) {
  using namespace rs::core::database;
  auto transport = std::make_unique<ScriptedBackendTransport>(
      ScriptedBackendTransport::ResponseMode::IncompleteLargeDataRow);
  auto* observed = transport.get();
  postgres::PgDatabaseConnection backend(std::move(transport));
  ConnectionSettings settings;
  settings.use_ssl = false;
  const auto result = backend.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::ResourceLimit);
  EXPECT_EQ(result.backend_error().operation, BackendOperation::Authenticate);
  EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
  EXPECT_EQ(observed->bytes_read(), 5u);
  EXPECT_EQ(observed->close_count(), 1u);
}

TEST(ResultBudgetTest, ExactCountsSucceedAndOverflowsReturnNoPartialResult) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const int dimension : {0, 1, 2, 3}) {
    for (const bool exact : {false, true}) {
      auto transport = std::make_unique<ScriptedBackendTransport>(Mode::OwnedResultCells);
      auto* observed = transport.get();
      postgres::PgDatabaseConnection backend(std::move(transport));
      ConnectionSettings settings;
      settings.use_ssl = false;
      if (dimension == 0) settings.result_limits.max_rows = exact ? 3 : 2;
      if (dimension == 1) settings.result_limits.max_cells = exact ? 3 : 2;
      if (dimension == 2) settings.result_limits.max_columns_per_description = exact ? 1 : 0;
      if (dimension == 3) settings.result_limits.max_results = exact ? 2 : 1;
      ASSERT_TRUE(backend.connect(settings));
      const auto result = backend.execute_query("bounded", rs::util::Deadline::max());
      EXPECT_EQ(exact, result.has_value());
      if (exact) {
        ASSERT_TRUE(result);
        EXPECT_EQ(result->rows.size(), 3u);
        EXPECT_EQ(result->additional_results.size(), 1u);
        EXPECT_TRUE(backend.is_connected());
      } else {
        ASSERT_TRUE(result.has_error());
        EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::ResourceLimit);
        EXPECT_EQ(result.backend_error().operation, BackendOperation::ExecuteDirect);
        EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
        EXPECT_FALSE(result.backend_error().native_state);
        EXPECT_FALSE(result.backend_error().retry_safe);
        EXPECT_FALSE(backend.is_connected());
        EXPECT_EQ(observed->close_count(), 1u);
        const auto sends = observed->send_count();
        EXPECT_FALSE(backend.execute_query("later", rs::util::Deadline::max()));
        EXPECT_EQ(observed->send_count(), sends);
      }
    }
  }
}


TEST(ResultBudgetTest, ParameterDescriptionAndAggregateMultiResultCountsAreBounded) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const bool exact : {false, true}) {
    auto transport = std::make_unique<ScriptedBackendTransport>(Mode::DescriptionOneParameter);
    postgres::PgDatabaseConnection backend(std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    settings.result_limits.max_columns_per_description = exact ? 1 : 0;
    ASSERT_TRUE(backend.connect(settings));
    const auto result = backend.describe_statement("bounded", {}, rs::util::Deadline::max());
    EXPECT_EQ(exact, result.has_value());
    if (exact) {
      ASSERT_TRUE(result);
      ASSERT_EQ(1u, result->normalized_parameter_types.size());
      EXPECT_EQ(ScalarType::Integer, result->normalized_parameter_types[0].type);
    } else {
      ASSERT_TRUE(result.has_error());
      EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::ResourceLimit);
      EXPECT_EQ(result.backend_error().operation, BackendOperation::Describe);
      EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
    }
  }
  for (const bool cells : {false, true}) {
    for (const bool exact : {false, true}) {
      postgres::PgDatabaseConnection backend(std::make_unique<ScriptedBackendTransport>(Mode::OwnedTwoResultSets));
      ConnectionSettings settings;
      settings.use_ssl = false;
      if (cells) settings.result_limits.max_cells = exact ? 6 : 5;
      else settings.result_limits.max_rows = exact ? 6 : 5;
      ASSERT_TRUE(backend.connect(settings));
      const auto result = backend.execute_query("bounded", rs::util::Deadline::max());
      EXPECT_EQ(exact, result.has_value());
      if (exact) {
        ASSERT_TRUE(result);
        ASSERT_EQ(1u, result->additional_results.size());
        EXPECT_EQ(result->rows.size(), 3u);
        EXPECT_EQ(result->additional_results[0].rows.size(), 3u);
      } else {
        ASSERT_TRUE(result.has_error());
        EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::ResourceLimit);
        EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
      }
    }
  }
}

TEST(ResultBudgetTest, ZeroDataBudgetsPermitNoDataAndInvalidResultLimitPreservesSession) {
  using namespace rs::core::database;
  auto transport = std::make_unique<ScriptedBackendTransport>(
      ScriptedBackendTransport::ResponseMode::DescriptionNoData);
  auto* observed = transport.get();
  postgres::PgDatabaseConnection backend(std::move(transport));
  ConnectionSettings settings;
  settings.use_ssl = false;
  settings.result_limits.max_rows = 0;
  settings.result_limits.max_cells = 0;
  settings.result_limits.max_columns_per_description = 0;
  ASSERT_TRUE(backend.connect(settings));
  const auto description = backend.describe_statement("bounded", {}, rs::util::Deadline::max());
  ASSERT_TRUE(description);
  EXPECT_TRUE(description->rows.empty());
  EXPECT_TRUE(description->columns.empty());
  EXPECT_TRUE(description->normalized_parameter_types.empty());
  const auto reads = observed->bytes_read();
  const auto connects = observed->connect_count();
  auto invalid = settings;
  invalid.result_limits.max_results = 0;
  const auto result = backend.connect(invalid);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::InvalidInput);
  EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Reusable);
  EXPECT_TRUE(backend.is_connected());
  EXPECT_EQ(observed->bytes_read(), reads);
  EXPECT_EQ(observed->connect_count(), connects);
  EXPECT_EQ(observed->close_count(), 0u);
}

TEST(ResultBudgetTest, MetadataNamesAndEntriesAccumulateAcrossResults) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const int dimension : {0, 1, 2}) {
    for (const bool exact : {false, true}) {
      auto transport = std::make_unique<ScriptedBackendTransport>(Mode::OwnedTwoResultSets);
      auto* observed = transport.get();
      postgres::PgDatabaseConnection backend(std::move(transport));
      ConnectionSettings settings;
      settings.use_ssl = false;
      if (dimension == 0) settings.result_limits.max_metadata_entries = exact ? 2 : 1;
      if (dimension == 1) settings.result_limits.max_column_name_bytes = exact ? 5 : 4;
      if (dimension == 2) settings.result_limits.max_metadata_name_bytes = exact ? 10 : 9;
      ASSERT_TRUE(backend.connect(settings));
      const auto result = backend.execute_query("bounded", rs::util::Deadline::max());
      EXPECT_EQ(exact, result.has_value());
      if (exact) {
        ASSERT_TRUE(result);
        EXPECT_EQ(result->columns[0].name, "value");
        ASSERT_EQ(result->additional_results.size(), 1u);
        EXPECT_EQ(result->additional_results[0].columns[0].name, "value");
      } else {
        ASSERT_TRUE(result.has_error());
        EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::ResourceLimit);
        EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
        EXPECT_EQ(observed->close_count(), 1u);
        const auto sends = observed->send_count();
        EXPECT_FALSE(backend.execute_query("later", rs::util::Deadline::max()));
        EXPECT_EQ(observed->send_count(), sends);
      }
    }
  }
}

TEST(ResultBudgetTest, ParameterEntriesUseAggregateMetadataBudget) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const bool exact : {false, true}) {
    postgres::PgDatabaseConnection backend(std::make_unique<ScriptedBackendTransport>(Mode::DescriptionOneParameter));
    ConnectionSettings settings;
    settings.use_ssl = false;
    settings.result_limits.max_metadata_entries = exact ? 1 : 0;
    settings.result_limits.max_metadata_name_bytes = 0;
    settings.result_limits.max_column_name_bytes = 0;
    ASSERT_TRUE(backend.connect(settings));
    const auto result = backend.describe_statement("bounded", {}, rs::util::Deadline::max());
    EXPECT_EQ(exact, result.has_value());
    if (exact) {
      ASSERT_TRUE(result);
      ASSERT_EQ(1u, result->normalized_parameter_types.size());
      EXPECT_EQ(ScalarType::Integer, result->normalized_parameter_types[0].type);
    } else {
      ASSERT_TRUE(result.has_error());
      EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::ResourceLimit);
      EXPECT_EQ(result.backend_error().operation, BackendOperation::Describe);
      EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
    }
  }
}

TEST(ResultBudgetTest, DiagnosticPayloadLimitRejectsQueryErrorBeforeBodyRead) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  constexpr char error[] = "SERROR\0C42601\0Msyntax error\0";
  for (const bool exact : {false, true}) {
    auto transport = std::make_unique<ScriptedBackendTransport>(Mode::OwnedErrorIdle);
    auto* observed = transport.get();
    postgres::PgDatabaseConnection backend(std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    settings.result_limits.max_diagnostic_bytes = exact ? sizeof(error) : sizeof(error) - 1;
    ASSERT_TRUE(backend.connect(settings));
    const auto startup_bytes = observed->bytes_read();
    const auto result = backend.execute_query("bounded", rs::util::Deadline::max());
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.backend_error().error_class, exact ? BackendErrorClass::Server : BackendErrorClass::ResourceLimit);
    if (exact) {
      EXPECT_EQ(result.backend_error().native_state, "42601");
      EXPECT_TRUE(backend.is_connected());
    } else {
      EXPECT_EQ(observed->bytes_read(), startup_bytes + 5);
      EXPECT_FALSE(result.backend_error().native_state);
      EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
      EXPECT_EQ(observed->close_count(), 1u);
    }
  }
}

TEST(ResultBudgetTest, DiagnosticLimitCoversAuthenticationErrorsAndStartupNotices) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  constexpr char error[] = "SFATAL\0C28P01\0Mpassword authentication failed\0";
  constexpr char notice[] = "SNOTICE\0C00000\0Mstartup notice\0";
  for (const bool authentication : {false, true}) {
    for (const bool exact : {false, true}) {
      auto transport = std::make_unique<ScriptedBackendTransport>(authentication ? Mode::AuthRejected : Mode::NoticeAfterAuthenticationOk);
      auto* observed = transport.get();
      postgres::PgDatabaseConnection backend(std::move(transport));
      ConnectionSettings settings;
      settings.use_ssl = false;
      const auto payload_bytes = authentication ? sizeof(error) : sizeof(notice);
      settings.result_limits.max_diagnostic_bytes = exact ? payload_bytes : payload_bytes - 1;
      const auto result = backend.connect(settings);
      if (exact && !authentication) {
        ASSERT_TRUE(result);
        EXPECT_TRUE(backend.is_connected());
      } else {
        ASSERT_TRUE(result.has_error());
        EXPECT_EQ(result.backend_error().error_class, exact ? BackendErrorClass::Authentication : BackendErrorClass::ResourceLimit);
        EXPECT_EQ(result.backend_error().operation, authentication ? BackendOperation::Authenticate : BackendOperation::Startup);
        EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
        EXPECT_EQ(observed->close_count(), 1u);
        if (!exact) {
          EXPECT_EQ(observed->bytes_read(), authentication ? 5u : 14u);
        }
      }
    }
  }
}

TEST(ResultBudgetTest, MalformedMetadataRetiresAndEmptyNamesFitZeroByteBudgets) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::UnterminatedColumnName, Mode::TruncatedColumnMetadata, Mode::TrailingColumnMetadata, Mode::EmptyColumnName}) {
    auto transport = std::make_unique<ScriptedBackendTransport>(mode);
    auto* observed = transport.get();
    postgres::PgDatabaseConnection backend(std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    if (mode == Mode::EmptyColumnName) {
      settings.result_limits.max_column_name_bytes = 0;
      settings.result_limits.max_metadata_name_bytes = 0;
    }
    ASSERT_TRUE(backend.connect(settings));
    const auto result = backend.execute_query("bounded", rs::util::Deadline::max());
    if (mode == Mode::EmptyColumnName) {
      ASSERT_TRUE(result);
      ASSERT_EQ(result->columns.size(), 1u);
      EXPECT_TRUE(result->columns[0].name.empty());
    } else {
      ASSERT_TRUE(result.has_error());
      EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::Protocol);
      EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
      EXPECT_EQ(observed->close_count(), 1u);
    }
  }
}

TEST(InputBudgetTest, SqlLimitPreflightsEveryRequestAndAllowsRecovery) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto operation : {BackendOperation::ExecuteDirect, BackendOperation::ExecutePrepared, BackendOperation::Describe}) {
    const bool describe = operation == BackendOperation::Describe;
    auto transport = std::make_unique<ScriptedBackendTransport>(describe ? Mode::DescriptionNoData : Mode::EmptyQueryResponse);
    auto* observed = transport.get();
    postgres::PgDatabaseConnection backend(std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    settings.input_limits.max_sql_bytes = 3;
    ASSERT_TRUE(backend.connect(settings));
    const auto invoke = [&](std::string_view sql) {
      if (operation == BackendOperation::ExecuteDirect) return backend.execute_query(sql, rs::util::Deadline::max());
      if (operation == BackendOperation::ExecutePrepared) return backend.execute_prepared(sql, std::span<const QueryParameter>{}, rs::util::Deadline::max());
      return backend.describe_statement(sql, {}, rs::util::Deadline::max());
    };
    const auto sends = observed->send_count();
    const auto reads = observed->bytes_read();
    const auto rejected = invoke("1234");
    ASSERT_TRUE(rejected.has_error());
    EXPECT_EQ(rejected.backend_error().error_class, BackendErrorClass::ResourceLimit);
    EXPECT_EQ(rejected.backend_error().operation, operation);
    EXPECT_EQ(rejected.backend_error().session_state, SessionState::Idle);
    EXPECT_EQ(rejected.backend_error().disposition, SessionDisposition::Reusable);
    EXPECT_FALSE(rejected.backend_error().native_state);
    EXPECT_FALSE(rejected.backend_error().retry_safe);
    EXPECT_EQ(observed->send_count(), sends);
    EXPECT_EQ(observed->bytes_read(), reads);
    EXPECT_EQ(observed->close_count(), 0u);
    EXPECT_TRUE(invoke("123"));
  }
}

TEST(InputBudgetTest, ParameterCountsValuesAndAggregateBytesAreBoundedBeforeEncoding) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  const std::vector<QueryParameter> parameters{
      {std::nullopt, QueryParameterType::Text}, {std::string{}, QueryParameterType::Text},
      {std::string{"ab"}, QueryParameterType::Text}, {std::string{"\0x", 2}, QueryParameterType::Binary}};
  for (const int dimension : {0, 1, 2}) {
    auto transport = std::make_unique<ScriptedBackendTransport>(Mode::PreparedCommand);
    auto* observed = transport.get();
    postgres::PgDatabaseConnection backend(std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    settings.input_limits.max_parameters = 4;
    settings.input_limits.max_parameter_bytes = 2;
    settings.input_limits.max_parameter_total_bytes = 4;
    ASSERT_TRUE(backend.connect(settings));
    auto over = parameters;
    if (dimension == 0) over.push_back({std::nullopt, QueryParameterType::Text});
    if (dimension == 1) over[3].value = std::string{"\0xy", 3};
    if (dimension == 2) over[1].value = "x";
    const auto sends = observed->send_count();
    const auto reads = observed->bytes_read();
    const auto rejected = backend.execute_prepared("?,?,?,?", over, rs::util::Deadline::max());
    ASSERT_TRUE(rejected.has_error());
    EXPECT_EQ(rejected.backend_error().error_class, BackendErrorClass::ResourceLimit);
    EXPECT_EQ(rejected.backend_error().disposition, SessionDisposition::Reusable);
    EXPECT_EQ(observed->send_count(), sends);
    EXPECT_EQ(observed->bytes_read(), reads);
    EXPECT_EQ(observed->close_count(), 0u);
    const auto result = backend.execute_prepared("?,?,?,?", parameters, rs::util::Deadline::max());
    ASSERT_TRUE(result);
    EXPECT_EQ(result->affected_rows, 1);
  }
}

TEST(InputBudgetTest, DescriptionParameterCountLimitAllowsExactBoundary) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  auto transport = std::make_unique<ScriptedBackendTransport>(Mode::DescriptionOneParameter);
  auto* observed = transport.get();
  postgres::PgDatabaseConnection backend(std::move(transport));
  ConnectionSettings settings;
  settings.use_ssl = false;
  settings.input_limits.max_parameters = 1;
  ASSERT_TRUE(backend.connect(settings));
  const std::vector<QueryParameterType> over{QueryParameterType::Text, QueryParameterType::Text};
  const auto sends = observed->send_count();
  const auto rejected = backend.describe_statement("?,?", over, rs::util::Deadline::max());
  ASSERT_TRUE(rejected.has_error());
  EXPECT_EQ(rejected.backend_error().error_class, BackendErrorClass::ResourceLimit);
  EXPECT_EQ(rejected.backend_error().operation, BackendOperation::Describe);
  EXPECT_EQ(rejected.backend_error().disposition, SessionDisposition::Reusable);
  EXPECT_EQ(observed->send_count(), sends);
  const std::vector<QueryParameterType> exact{QueryParameterType::Int32};
  EXPECT_TRUE(backend.describe_statement("?", exact, rs::util::Deadline::max()));
}

TEST(InputBudgetTest, RejectionPreservesTransactionAndDisconnectedPrecedence) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::OwnedErrorTransaction, Mode::OwnedErrorAborted}) {
    auto transport = std::make_unique<ScriptedBackendTransport>(mode);
    auto* observed = transport.get();
    postgres::PgDatabaseConnection backend(std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    settings.input_limits.max_sql_bytes = 6;
    ASSERT_TRUE(backend.connect(settings));
    EXPECT_FALSE(backend.execute_query("broken", rs::util::Deadline::max()));
    const auto sends = observed->send_count();
    const auto reads = observed->bytes_read();
    const auto rejected = backend.execute_query("too long", rs::util::Deadline::max());
    ASSERT_TRUE(rejected.has_error());
    EXPECT_EQ(rejected.backend_error().error_class, BackendErrorClass::ResourceLimit);
    EXPECT_EQ(rejected.backend_error().session_state, mode == Mode::OwnedErrorTransaction ? SessionState::Transaction : SessionState::FailedTransaction);
    EXPECT_EQ(rejected.backend_error().disposition, SessionDisposition::ResetRequired);
    EXPECT_EQ(observed->send_count(), sends);
    EXPECT_EQ(observed->bytes_read(), reads);
    EXPECT_EQ(observed->close_count(), 0u);
    backend.disconnect();
    const auto disconnected = backend.execute_query("too long", rs::util::Deadline::max());
    ASSERT_TRUE(disconnected.has_error());
    EXPECT_EQ(disconnected.backend_error().error_class, BackendErrorClass::NotConnected);
  }
}

TEST(InputBudgetTest, ConnectionFieldsAreBoundedBeforeTransportAndReconnectMutation) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  using Field = std::string ConnectionSettings::*;
  for (const Field field : {&ConnectionSettings::host, &ConnectionSettings::user,
                           &ConnectionSettings::password, &ConnectionSettings::database}) {
    auto transport = std::make_unique<ScriptedBackendTransport>(Mode::ValidStartup);
    auto* observed = transport.get();
    postgres::PgDatabaseConnection backend(std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    settings.input_limits.max_connection_field_bytes = 3;
    settings.*field = "1234";
    const auto rejected = backend.connect(settings);
    ASSERT_TRUE(rejected.has_error());
    EXPECT_EQ(rejected.backend_error().error_class, BackendErrorClass::ResourceLimit);
    EXPECT_EQ(rejected.backend_error().operation, BackendOperation::Connect);
    EXPECT_EQ(rejected.backend_error().disposition, SessionDisposition::Retire);
    EXPECT_EQ(observed->connect_count(), 0u);
    EXPECT_EQ(observed->send_count(), 0u);
    EXPECT_EQ(observed->bytes_read(), 0u);
    EXPECT_EQ(observed->close_count(), 0u);
    settings.*field = "123";
    ASSERT_TRUE(backend.connect(settings));
    const auto reads = observed->bytes_read();
    const auto sends = observed->send_count();
    settings.*field = "1234";
    const auto reconnect = backend.connect(settings);
    ASSERT_TRUE(reconnect.has_error());
    EXPECT_EQ(reconnect.backend_error().error_class, BackendErrorClass::ResourceLimit);
    EXPECT_EQ(reconnect.backend_error().disposition, SessionDisposition::Reusable);
    EXPECT_TRUE(backend.is_connected());
    EXPECT_EQ(observed->connect_count(), 1u);
    EXPECT_EQ(observed->bytes_read(), reads);
    EXPECT_EQ(observed->send_count(), sends);
    EXPECT_EQ(observed->close_count(), 0u);
  }
  for (const Field field : {&ConnectionSettings::ssl_ca_file, &ConnectionSettings::ssl_ca_dir}) {
    auto transport = std::make_unique<ScriptedBackendTransport>();
    auto* observed = transport.get();
    postgres::PgDatabaseConnection backend(std::move(transport));
    ConnectionSettings settings;
    settings.input_limits.max_connection_field_bytes = 3;
    settings.*field = "1234";
    const auto result = backend.connect(settings);
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::ResourceLimit);
    EXPECT_EQ(observed->connect_count(), 0u);
    EXPECT_EQ(observed->send_count(), 0u);
  }
}

TEST(InputBudgetTest, ExcessiveConfiguredCeilingsPreserveConnectedSession) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  using Limit = std::size_t InputLimits::*;
  auto transport = std::make_unique<ScriptedBackendTransport>(Mode::ValidStartup);
  auto* observed = transport.get();
  postgres::PgDatabaseConnection backend(std::move(transport));
  ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(backend.connect(settings));
  const auto reads = observed->bytes_read();
  const auto sends = observed->send_count();
  for (const Limit field : {&InputLimits::max_sql_bytes, &InputLimits::max_parameters,
                           &InputLimits::max_parameter_bytes, &InputLimits::max_parameter_total_bytes,
                           &InputLimits::max_connection_field_bytes, &InputLimits::max_request_wire_bytes, &InputLimits::max_startup_wire_bytes, &InputLimits::max_auth_wire_bytes}) {
    auto invalid = settings;
    invalid.input_limits.*field = static_cast<std::size_t>(-1);
    const auto result = backend.connect(invalid);
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::InvalidInput);
    EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Reusable);
    EXPECT_TRUE(backend.is_connected());
    EXPECT_EQ(observed->connect_count(), 1u);
    EXPECT_EQ(observed->send_count(), sends);
    EXPECT_EQ(observed->bytes_read(), reads);
    EXPECT_EQ(observed->close_count(), 0u);
  }
}

TEST(InputBudgetTest, ZeroBudgetsAllowEmptySqlNullAndEmptyParameters) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  postgres::PgDatabaseConnection direct(std::make_unique<ScriptedBackendTransport>(Mode::EmptyQueryResponse));
  ConnectionSettings settings;
  settings.use_ssl = false;
  settings.input_limits.max_sql_bytes = 0;
  settings.input_limits.max_parameters = 0;
  settings.input_limits.max_parameter_bytes = 0;
  settings.input_limits.max_parameter_total_bytes = 0;
  settings.input_limits.max_connection_field_bytes = 0;
  ASSERT_TRUE(direct.connect(settings));
  EXPECT_TRUE(direct.execute_query("", rs::util::Deadline::max()));
  auto transport = std::make_unique<ScriptedBackendTransport>(Mode::PreparedCommand);
  postgres::PgDatabaseConnection prepared(std::move(transport));
  settings.input_limits.max_sql_bytes = 7;
  settings.input_limits.max_parameters = 4;
  ASSERT_TRUE(prepared.connect(settings));
  const std::vector<QueryParameter> params{
      {std::nullopt, QueryParameterType::Text}, {std::string{}, QueryParameterType::Text},
      {std::string{}, QueryParameterType::Text}, {std::string{}, QueryParameterType::Binary}};
  EXPECT_TRUE(prepared.execute_prepared("?,?,?,?", params, rs::util::Deadline::max()));
}

namespace {
class AllocationFaultParser final : public rs::core::database::postgres::PgProtocolParser {
 public:
  enum class Stage { None, StartupEncoding, StartupParse, Direct, Prepared, Describe, Parse, Extract };
  Stage stage{Stage::None};
  void fail(Stage point) {
    if (stage == point) {
      stage = Stage::None;
      throw std::bad_alloc{};
    }
  }
  std::vector<std::byte> create_startup_message(const std::string& user, const std::string& database,
      const std::map<std::string, std::string>& params, std::size_t limit) override {
    fail(Stage::StartupEncoding);
    return PgProtocolParser::create_startup_message(user, database, params, limit);
  }
  std::vector<std::byte> create_simple_query(std::string_view sql, std::size_t limit) override {
    fail(Stage::Direct);
    return PgProtocolParser::create_simple_query(sql, limit);
  }
  std::vector<std::byte> create_prepared_query(std::string_view sql,
      std::span<const rs::core::database::QueryParameter> params, std::size_t limit) override {
    fail(Stage::Prepared);
    return PgProtocolParser::create_prepared_query(sql, params, limit);
  }
  std::vector<std::byte> create_statement_description(std::string_view sql,
      std::span<const rs::core::database::QueryParameterType> types, std::size_t limit) override {
    fail(Stage::Describe);
    return PgProtocolParser::create_statement_description(sql, types, limit);
  }
  rs::core::database::Message parse_message(const std::vector<std::byte>& bytes) override {
    fail(Stage::StartupParse);
    fail(Stage::Parse);
    return PgProtocolParser::parse_message(bytes);
  }
  rs::core::database::ParsedQueryResult extract_query_result(const std::vector<rs::core::database::Message>& messages) override {
    fail(Stage::Extract);
    return PgProtocolParser::extract_query_result(messages);
  }
};
}

TEST(AllocationBoundaryTest, EncodingFailuresPreserveSessionAndAllowRecovery) {
  using namespace rs::core::database;
  using Stage = AllocationFaultParser::Stage;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto stage : {Stage::Direct, Stage::Prepared, Stage::Describe}) {
    auto parser = std::make_unique<AllocationFaultParser>();
    auto* fault = parser.get();
    auto transport = std::make_unique<ScriptedBackendTransport>(stage == Stage::Describe ? Mode::DescriptionNoData : Mode::EmptyQueryResponse);
    auto* observed = transport.get();
    GenericDatabaseConnection backend(std::move(parser), std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    ASSERT_TRUE(backend.connect(settings));
    const auto invoke = [&] {
      if (stage == Stage::Direct) return backend.execute_query("", rs::util::Deadline::max());
      if (stage == Stage::Prepared) return backend.execute_prepared("", std::span<const QueryParameter>{}, rs::util::Deadline::max());
      return backend.describe_statement("", {}, rs::util::Deadline::max());
    };
    fault->stage = stage;
    const auto sends = observed->send_count();
    const auto reads = observed->bytes_read();
    const auto result = invoke();
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::AllocationFailure);
    EXPECT_EQ(result.backend_error().operation, stage == Stage::Direct ? BackendOperation::ExecuteDirect :
        stage == Stage::Prepared ? BackendOperation::ExecutePrepared : BackendOperation::Describe);
    EXPECT_EQ(result.backend_error().session_state, SessionState::Idle);
    EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Reusable);
    EXPECT_TRUE(result.error_message().empty());
    EXPECT_FALSE(result.backend_error().native_state);
    EXPECT_EQ(observed->send_count(), sends);
    EXPECT_EQ(observed->bytes_read(), reads);
    EXPECT_EQ(observed->close_count(), 0u);
    EXPECT_TRUE(invoke());
  }
}

TEST(AllocationBoundaryTest, ResponseFailuresRetireWithoutPartialResults) {
  using namespace rs::core::database;
  using Stage = AllocationFaultParser::Stage;
  for (const auto stage : {Stage::None, Stage::Parse, Stage::Extract}) {
    auto parser = std::make_unique<AllocationFaultParser>();
    auto* fault = parser.get();
    auto transport = std::make_unique<ScriptedBackendTransport>(stage == Stage::None ?
        ScriptedBackendTransport::ResponseMode::QueryAllocationFailure : ScriptedBackendTransport::ResponseMode::OwnedTwoResultSets);
    auto* observed = transport.get();
    GenericDatabaseConnection backend(std::move(parser), std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    ASSERT_TRUE(backend.connect(settings));
    fault->stage = stage;
    const auto result = backend.execute_query("bounded", rs::util::Deadline::max());
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::AllocationFailure);
    EXPECT_EQ(result.backend_error().session_state, SessionState::Disconnected);
    EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
    EXPECT_FALSE(result.backend_error().retry_safe);
    EXPECT_EQ(observed->close_count(), 1u);
    const auto sends = observed->send_count();
    EXPECT_FALSE(backend.execute_query("later", rs::util::Deadline::max()));
    EXPECT_EQ(observed->send_count(), sends);
  }
}

TEST(AllocationBoundaryTest, StartupFailuresRespectPhaseAndCleanup) {
  using namespace rs::core::database;
  using Stage = AllocationFaultParser::Stage;
  for (const auto stage : {Stage::StartupEncoding, Stage::StartupParse}) {
    auto parser = std::make_unique<AllocationFaultParser>();
    auto* fault = parser.get();
    fault->stage = stage;
    auto transport = std::make_unique<ScriptedBackendTransport>();
    auto* observed = transport.get();
    GenericDatabaseConnection backend(std::move(parser), std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    const auto result = backend.connect(settings);
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::AllocationFailure);
    EXPECT_EQ(result.backend_error().operation, stage == Stage::StartupEncoding ? BackendOperation::Connect : BackendOperation::Authenticate);
    EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
    EXPECT_EQ(observed->connect_count(), stage == Stage::StartupEncoding ? 0u : 1u);
    EXPECT_EQ(observed->close_count(), stage == Stage::StartupEncoding ? 0u : 1u);
    if (stage == Stage::StartupEncoding) {
      EXPECT_TRUE(backend.connect(settings));
    }
  }
}

TEST(AllocationBoundaryTest, StartupEncodingFailurePreservesExistingSession) {
  using namespace rs::core::database;
  auto parser = std::make_unique<AllocationFaultParser>();
  auto* fault = parser.get();
  auto transport = std::make_unique<ScriptedBackendTransport>();
  auto* observed = transport.get();
  GenericDatabaseConnection backend(std::move(parser), std::move(transport));
  ConnectionSettings settings;
  settings.use_ssl = false;
  ASSERT_TRUE(backend.connect(settings));
  fault->stage = AllocationFaultParser::Stage::StartupEncoding;
  const auto reads = observed->bytes_read();
  const auto sends = observed->send_count();
  const auto result = backend.connect(settings);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::AllocationFailure);
  EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Reusable);
  EXPECT_EQ(observed->connect_count(), 1u);
  EXPECT_EQ(observed->send_count(), sends);
  EXPECT_EQ(observed->bytes_read(), reads);
  EXPECT_EQ(observed->close_count(), 0u);
  EXPECT_TRUE(backend.is_connected());
}

TEST(InputBudgetTest, EncodedWireLimitsPreflightAndPreserveSessionAcrossAllOperations) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto operation : {BackendOperation::ExecuteDirect, BackendOperation::ExecutePrepared, BackendOperation::Describe}) {
    for (const bool exact : {false, true}) {
      auto transport = std::make_unique<ScriptedBackendTransport>(operation == BackendOperation::Describe ? Mode::DescriptionNoData : Mode::EmptyQueryResponse);
      auto* observed = transport.get();
      GenericDatabaseConnection backend(std::make_unique<postgres::PgProtocolParser>(), std::move(transport));
      ConnectionSettings settings;
      settings.use_ssl = false;
      const std::size_t wire_bytes = operation == BackendOperation::ExecuteDirect ? 6 :
          operation == BackendOperation::ExecutePrepared ? 51 : 21;
      settings.input_limits.max_request_wire_bytes = exact ? wire_bytes : wire_bytes - 1;
      ASSERT_TRUE(backend.connect(settings));
      const auto sends = observed->send_count();
      const auto reads = observed->bytes_read();
      const auto result = operation == BackendOperation::ExecuteDirect ? backend.execute_query("", rs::util::Deadline::max()) :
          operation == BackendOperation::ExecutePrepared ? backend.execute_prepared("", std::span<const QueryParameter>{}, rs::util::Deadline::max()) :
          backend.describe_statement("", {}, rs::util::Deadline::max());
      EXPECT_EQ(exact, result.has_value());
      if (!exact) {
        ASSERT_TRUE(result.has_error());
        EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::ResourceLimit);
        EXPECT_EQ(result.backend_error().operation, operation);
        EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Reusable);
        EXPECT_EQ(observed->send_count(), sends);
        EXPECT_EQ(observed->bytes_read(), reads);
        EXPECT_EQ(observed->close_count(), 0u);
        EXPECT_TRUE(backend.is_connected());
      }
    }
  }
}

TEST(InputBudgetTest, StartupWireLimitRejectsBeforeConnectAndPreservesExistingOwner) {
  using namespace rs::core::database;
  auto transport = std::make_unique<ScriptedBackendTransport>();
  auto* observed = transport.get();
  GenericDatabaseConnection backend(std::make_unique<postgres::PgProtocolParser>(), std::move(transport));
  ConnectionSettings settings;
  settings.use_ssl = false;
  settings.input_limits.max_startup_wire_bytes = 0;
  const auto rejected = backend.connect(settings);
  ASSERT_TRUE(rejected.has_error());
  EXPECT_EQ(rejected.backend_error().error_class, BackendErrorClass::ResourceLimit);
  EXPECT_EQ(observed->connect_count(), 0u);
  EXPECT_EQ(observed->send_count(), 0u);
  settings.input_limits.max_startup_wire_bytes = 1024;
  ASSERT_TRUE(backend.connect(settings));
  const auto sends = observed->send_count();
  const auto reads = observed->bytes_read();
  settings.input_limits.max_startup_wire_bytes = 0;
  const auto reconnect = backend.connect(settings);
  ASSERT_TRUE(reconnect.has_error());
  EXPECT_EQ(reconnect.backend_error().disposition, SessionDisposition::Reusable);
  EXPECT_EQ(observed->connect_count(), 1u);
  EXPECT_EQ(observed->send_count(), sends);
  EXPECT_EQ(observed->bytes_read(), reads);
  EXPECT_EQ(observed->close_count(), 0u);
}

TEST(InputBudgetTest, AuthenticationWireLimitRetiresWithoutSendingOversizedPasswordPacket) {
  using namespace rs::core::database;
  for (const bool exact : {false, true}) {
    auto transport = std::make_unique<ScriptedBackendTransport>(ScriptedBackendTransport::ResponseMode::Md5Authentication);
    auto* observed = transport.get();
    GenericDatabaseConnection backend(std::make_unique<postgres::PgProtocolParser>(), std::move(transport));
    ConnectionSettings settings;
    settings.use_ssl = false;
    settings.password = "secret";
    settings.input_limits.max_auth_wire_bytes = exact ? 41 : 40;
    const auto result = backend.connect(settings);
    EXPECT_EQ(exact, result.has_value());
    if (!exact) {
      ASSERT_TRUE(result.has_error());
      EXPECT_EQ(result.backend_error().error_class, BackendErrorClass::ResourceLimit);
      EXPECT_EQ(result.backend_error().operation, BackendOperation::Authenticate);
      EXPECT_EQ(result.backend_error().disposition, SessionDisposition::Retire);
      EXPECT_EQ(observed->send_count(), 1u);
      EXPECT_EQ(observed->close_count(), 1u);
    } else {
      EXPECT_EQ(observed->send_count(), 2u);
      EXPECT_TRUE(backend.is_connected());
    }
  }
}


TEST(NormalizedColumnTest, SessionOwnsPrimaryAdditionalAndDescriptionMetadata) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const bool describe : {false, true}) {
    QueryResult snapshot;
    {
      postgres::PgDatabaseConnection backend(std::make_unique<ScriptedBackendTransport>(describe ? Mode::DescriptionOneColumn : Mode::OwnedTwoResultSets));
      ConnectionSettings settings; settings.use_ssl = false;
      ASSERT_TRUE(backend.connect(settings));
      auto result = describe
          ? backend.describe_statement("SELECT value", {}, rs::util::Deadline::max())
          : backend.execute_query("SELECT value; SELECT value", rs::util::Deadline::max());
      ASSERT_TRUE(result);
      snapshot = std::move(*result);
      backend.disconnect();
    }
    ASSERT_EQ(1u, snapshot.columns.size());
    ASSERT_TRUE(snapshot.columns[0].normalized_type);
    EXPECT_EQ("value", snapshot.columns[0].name);
    EXPECT_EQ(ScalarType::VarChar, snapshot.columns[0].normalized_type->type);
    EXPECT_TRUE(snapshot.columns[0].normalized_type->known);
    EXPECT_EQ(0u, snapshot.columns[0].normalized_type->column_size);
    if (describe) {
      EXPECT_TRUE(snapshot.rows.empty()); EXPECT_TRUE(snapshot.additional_results.empty());
    } else {
      ASSERT_EQ(1u, snapshot.additional_results.size());
      ASSERT_EQ(1u, snapshot.additional_results[0].columns.size());
      ASSERT_TRUE(snapshot.additional_results[0].columns[0].normalized_type);
      EXPECT_EQ(ScalarType::VarChar, snapshot.additional_results[0].columns[0].normalized_type->type);
      ASSERT_EQ(3u, snapshot.rows.size());
      EXPECT_EQ(std::nullopt, snapshot.rows[0][0]);
      EXPECT_EQ(std::optional<std::string>(""), snapshot.rows[1][0]);
      EXPECT_EQ(std::optional<std::string>("abc"), snapshot.rows[2][0]);
    }
  }
}


namespace {
class ParameterNormalizationConnection final : public rs::core::database::GenericDatabaseConnection {
 public:
  ParameterNormalizationConnection()
      : GenericDatabaseConnection(
          std::make_unique<rs::core::database::postgres::PgProtocolParser>(),
          std::make_unique<ScriptedBackendTransport>(ScriptedBackendTransport::ResponseMode::DescriptionRepeatedParameter)) {}
  bool incomplete{};
  bool fail{};
  bool passive_limit{};
  rs::util::Deadline observed{};
  int lookups{};
  rs::core::database::BackendResult<rs::core::database::ResolvedTypeMap> resolve_types(
      std::span<const std::uint32_t> ids, rs::util::Deadline deadline) override {
    using namespace rs::core::database;
    observed = deadline; ++lookups;
    if (passive_limit) {
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::ResourceLimit), "local lookup input limit"};
      error.session_state = session_state(); error.disposition = SessionDisposition::Reusable;
      return error;
    }
    if (fail) {
      BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "type lookup failed"};
      error.session_state = session_state(); error.disposition = SessionDisposition::Reusable;
      error.native_state = "22012";
      return error;
    }
    ResolvedTypeMap resolved;
    if (!incomplete) {
      for (const auto id : ids) resolved.emplace(id, NativeTypeInfo{ScalarType::Numeric, 18, 2, true});
    }
    return resolved;
  }
};
}

TEST(NormalizedParameterTest, BackendOwnsResolvedDescriptionsAndReusesAbsoluteDeadline) {
  using namespace rs::core::database;
  QueryResult snapshot;
  {
    ParameterNormalizationConnection backend;
    ConnectionSettings settings; settings.use_ssl = false;
    ASSERT_TRUE(backend.connect(settings));
    const auto deadline = rs::util::make_deadline(std::chrono::seconds(2));
    const QueryParameterType hints[]{QueryParameterType::Numeric};
    auto result = backend.describe_statement("SELECT ?", hints, deadline);
    ASSERT_TRUE(result); EXPECT_EQ(deadline, backend.observed); EXPECT_EQ(1, backend.lookups);
    snapshot = std::move(*result);
    backend.disconnect();
  }
  ASSERT_EQ(1u, snapshot.normalized_parameter_types.size());
  EXPECT_EQ(ScalarType::Numeric, snapshot.normalized_parameter_types[0].type);
  EXPECT_EQ(18u, snapshot.normalized_parameter_types[0].column_size);
  EXPECT_EQ(2, snapshot.normalized_parameter_types[0].decimal_digits);
}

TEST(NormalizedParameterTest, IncompleteAndFailedResolutionReturnNoPartialResultAndRecover) {
  using namespace rs::core::database;
  for (const bool resolver_error : {false, true}) {
    ParameterNormalizationConnection backend;
    ConnectionSettings settings; settings.use_ssl = false;
    ASSERT_TRUE(backend.connect(settings));
    backend.incomplete = !resolver_error; backend.fail = resolver_error;
    const QueryParameterType hints[]{QueryParameterType::Numeric};
    const auto deadline = rs::util::make_deadline(std::chrono::seconds(2));
    auto result = backend.describe_statement("SELECT ?", hints, deadline);
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(BackendOperation::ResolveTypes, result.backend_error().operation);
    EXPECT_EQ(resolver_error ? BackendErrorClass::Server : BackendErrorClass::InvalidMetadata,
        result.backend_error().error_class);
    EXPECT_EQ(SessionDisposition::Reusable, result.backend_error().disposition);
    EXPECT_EQ(SessionState::Idle, result.backend_error().session_state);
    EXPECT_EQ(deadline, backend.observed); EXPECT_TRUE(backend.is_connected());
    backend.incomplete = false; backend.fail = false;
    auto recovered = backend.describe_statement("SELECT ?", hints, deadline);
    ASSERT_TRUE(recovered); ASSERT_EQ(1u, recovered->normalized_parameter_types.size());
    EXPECT_EQ(2, backend.lookups);
  }
}


TEST(NormalizedCellTest, BackendReturnsCanonicalOwnedCellsAndDeferredEncodingErrors) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const bool parser_composition : {false, true}) {
    SCOPED_TRACE(parser_composition ? "Generic/PgParser" : "PgSession");
    for (const bool malformed : {false, true}) {
      QueryResult snapshot;
      {
        auto transport = std::make_unique<ScriptedBackendTransport>(
            malformed ? Mode::MalformedNativeCells : Mode::NativeCells);
        std::unique_ptr<GenericDatabaseConnection> owned;
        if (parser_composition) {
          owned = std::make_unique<GenericDatabaseConnection>(
              std::make_unique<postgres::PgProtocolParser>(), std::move(transport));
        } else {
          owned = std::make_unique<postgres::PgDatabaseConnection>(std::move(transport));
        }
        auto& backend = *owned;
        ConnectionSettings settings; settings.use_ssl = false;
        ASSERT_TRUE(backend.connect(settings));
        auto result = backend.execute_query("SELECT octets, flag; SELECT octets, flag", rs::util::Deadline::max());
        ASSERT_TRUE(result); EXPECT_TRUE(backend.is_connected());
        snapshot = std::move(*result); backend.disconnect();
      }
      ASSERT_EQ(1u, snapshot.additional_results.size());
      for (const auto* result : {&snapshot, &snapshot.additional_results[0]}) {
        ASSERT_EQ(3u, result->rows.size());
        ASSERT_EQ(2u, result->rows[0].size());
        if (malformed) {
          EXPECT_EQ((std::vector<CellEncodingError>{{0, 0}, {0, 1}}), result->cell_errors);
          EXPECT_EQ(std::optional<std::string>(""), result->rows[0][0]);
          EXPECT_EQ(std::optional<std::string>(""), result->rows[0][1]);
        } else {
          EXPECT_TRUE(result->cell_errors.empty());
          EXPECT_EQ(std::optional<std::string>(std::string("\0\xff\\", 3)), result->rows[0][0]);
          EXPECT_EQ(std::optional<std::string>("1"), result->rows[0][1]);
        }
        EXPECT_EQ(std::nullopt, result->rows[1][0]); EXPECT_EQ(std::nullopt, result->rows[1][1]);
        EXPECT_EQ(std::optional<std::string>(""), result->rows[2][0]);
        EXPECT_EQ(std::optional<std::string>("0"), result->rows[2][1]);
      }
    }
  }
}

TEST(NormalizedCellTest, TextUtf8IsValidatedBeforeReturningOwnedSnapshots) {
  using namespace rs::core::database;
  class LongTextParser final : public postgres::PgProtocolParser {
    NativeTypeInfo describe_type(std::uint32_t id, std::int16_t size, std::int32_t modifier) const override {
      auto type = PgProtocolParser::describe_type(id, size, modifier);
      if (id == 25) type.type = ScalarType::LongVarChar;
      return type;
    }
  };
  for (const int composition : {0, 1, 2}) {
    SCOPED_TRACE(composition);
    QueryResult snapshot;
    {
      auto transport = std::make_unique<ScriptedBackendTransport>(ScriptedBackendTransport::ResponseMode::Utf8TextCells);
      std::unique_ptr<GenericDatabaseConnection> owned;
      if (composition == 2) {
        owned = std::make_unique<GenericDatabaseConnection>(std::make_unique<LongTextParser>(), std::move(transport));
      } else if (composition == 1) {
        owned = std::make_unique<GenericDatabaseConnection>(std::make_unique<postgres::PgProtocolParser>(), std::move(transport));
      } else {
        owned = std::make_unique<postgres::PgDatabaseConnection>(std::move(transport));
      }
      ConnectionSettings settings; settings.use_ssl = false;
      ASSERT_TRUE(owned->connect(settings));
      auto result = owned->execute_query("SELECT text; SELECT text", rs::util::Deadline::max());
      ASSERT_TRUE(result); EXPECT_EQ(SessionState::Idle, owned->session_state());
      snapshot = std::move(*result);
      auto recovered = owned->execute_query("SELECT 0", rs::util::Deadline::max());
      ASSERT_TRUE(recovered); EXPECT_TRUE(recovered->cell_errors.empty());
      EXPECT_TRUE(owned->is_connected()); owned->disconnect();
    }
    ASSERT_EQ(1u, snapshot.additional_results.size());
    for (const auto* result : {&snapshot, &snapshot.additional_results[0]}) {
      ASSERT_EQ(10u, result->rows.size()); ASSERT_EQ(15u, result->cell_errors.size());
      for (std::size_t column = 0; column < 3; ++column) {
        EXPECT_EQ(std::optional<std::string>("ascii"), result->rows[0][column]);
        EXPECT_EQ(std::optional<std::string>(""), result->rows[1][column]);
        EXPECT_FALSE(result->rows[2][column]);
        EXPECT_EQ(std::optional<std::string>("\xe2\x82\xac\xf0\x9f\x98\x80"), result->rows[3][column]);
        EXPECT_EQ(std::optional<std::string>(std::string("a\0\xf4\x8f\xbf\xbf", 6)), result->rows[4][column]);
        for (std::size_t row = 5; row < 10; ++row) {
          EXPECT_EQ((CellEncodingError{row, column}), result->cell_errors[(row - 5) * 3 + column]);
          EXPECT_EQ(std::optional<std::string>(""), result->rows[row][column]);
        }
      }
    }
  }
}

TEST(NormalizedMetadataTest, Utf8NamesAreOwnedAndMalformedNamesRejectAllResultsWithoutRetirement) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::Utf8ColumnNames, Mode::MalformedColumnName, Mode::MalformedAdditionalColumnName}) {
    postgres::PgDatabaseConnection backend(std::make_unique<ScriptedBackendTransport>(mode));
    ConnectionSettings settings; settings.use_ssl = false;
    ASSERT_TRUE(backend.connect(settings));
    auto result = backend.execute_query("SELECT name; SELECT name", rs::util::Deadline::max());
    if (mode == Mode::Utf8ColumnNames) {
      ASSERT_TRUE(result); ASSERT_EQ(1u, result->additional_results.size());
      const auto name = result->columns[0].name;
      EXPECT_EQ("\xe2\x82\xac\xf0\x9f\x98\x80", name);
      EXPECT_EQ(name, result->additional_results[0].columns[0].name);
    } else {
      ASSERT_FALSE(result);
      EXPECT_EQ(BackendErrorClass::InvalidMetadata, result.backend_error().error_class);
      EXPECT_EQ(SessionDisposition::Reusable, result.backend_error().disposition);
      EXPECT_EQ(SessionState::Idle, result.backend_error().session_state);
      EXPECT_EQ("Data source returned invalid result metadata", result.error_message());
    }
    EXPECT_TRUE(backend.is_connected());
    ASSERT_TRUE(backend.execute_query("SELECT 0", rs::util::Deadline::max()));
    backend.disconnect();
    if (result) {
      EXPECT_EQ("\xe2\x82\xac\xf0\x9f\x98\x80", result->columns[0].name);
    }
  }
}

TEST(NormalizedParameterTest, PassiveLookupLimitPreservesDrainedSessionAndRecovers) {
  using namespace rs::core::database;
  ParameterNormalizationConnection backend;
  ConnectionSettings settings; settings.use_ssl = false;
  ASSERT_TRUE(backend.connect(settings));
  backend.passive_limit = true;
  const auto deadline = rs::util::make_deadline(std::chrono::seconds(2));
  const QueryParameterType hints[]{QueryParameterType::Numeric};
  auto result = backend.describe_statement("SELECT ?", hints, deadline);
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(BackendErrorClass::ResourceLimit, result.backend_error().error_class);
  EXPECT_EQ(BackendOperation::ResolveTypes, result.backend_error().operation);
  EXPECT_EQ(SessionState::Idle, result.backend_error().session_state);
  EXPECT_EQ(SessionDisposition::Reusable, result.backend_error().disposition);
  EXPECT_TRUE(backend.is_connected());
  EXPECT_EQ(deadline, backend.observed);
  backend.passive_limit = false;
  auto recovered = backend.describe_statement("SELECT ?", hints, deadline);
  ASSERT_TRUE(recovered);
  ASSERT_EQ(1u, recovered->normalized_parameter_types.size());
}

TEST(NormalizedMetadataTest, NestedPrivateResultsAreRejectedAfterDrainAndRecover) {
  using namespace rs::core::database;
  class NestedParser final : public postgres::PgProtocolParser {
   public:
    bool nested = true;
    ParsedQueryResult extract_query_result(const std::vector<Message>& messages) override {
      auto result = PgProtocolParser::extract_query_result(messages);
      if (nested) {
        result.additional_results.emplace_back();
        result.additional_results.back().additional_results.emplace_back();
        nested = false;
      }
      return result;
    }
  };
  GenericDatabaseConnection backend(std::make_unique<NestedParser>(),
      std::make_unique<ScriptedBackendTransport>(ScriptedBackendTransport::ResponseMode::Utf8ColumnNames));
  ConnectionSettings settings; settings.use_ssl = false;
  ASSERT_TRUE(backend.connect(settings));
  auto result = backend.execute_query("SELECT value; SELECT value", rs::util::Deadline::max());
  ASSERT_TRUE(result.has_error());
  EXPECT_EQ(BackendErrorClass::InvalidMetadata, result.backend_error().error_class);
  EXPECT_EQ(SessionDisposition::Reusable, result.backend_error().disposition);
  EXPECT_TRUE(backend.is_connected());
  ASSERT_TRUE(backend.execute_query("SELECT 0", rs::util::Deadline::max()));
}

TEST(NormalizedMetadataTest, ContradictoryPrivateErrorItemsRejectAfterDrainAndRecover) {
  using namespace rs::core::database;
  class ContradictoryParser final : public postgres::PgProtocolParser {
   public:
    explicit ContradictoryParser(bool primary) : primary_(primary) {}
    ParsedQueryResult extract_query_result(const std::vector<Message>& messages) override {
      auto result = PgProtocolParser::extract_query_result(messages);
      if (corrupt_) {
        auto& item = primary_ ? result : result.additional_results.at(0);
        item.error.emplace(rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "contradictory error");
        corrupt_ = false;
      }
      return result;
    }
   private:
    bool primary_;
    bool corrupt_ = true;
  };
  for (const bool primary : {false, true}) {
    GenericDatabaseConnection backend(std::make_unique<ContradictoryParser>(primary),
        std::make_unique<ScriptedBackendTransport>(ScriptedBackendTransport::ResponseMode::Utf8ColumnNames));
    ConnectionSettings settings; settings.use_ssl = false;
    ASSERT_TRUE(backend.connect(settings));
    auto result = backend.execute_query("SELECT value; SELECT value", rs::util::Deadline::max());
    ASSERT_TRUE(result.has_error());
    EXPECT_EQ(BackendErrorClass::InvalidMetadata, result.backend_error().error_class);
    EXPECT_EQ(SessionState::Idle, result.backend_error().session_state);
    EXPECT_EQ(SessionDisposition::Reusable, result.backend_error().disposition);
    EXPECT_EQ("Data source returned invalid execution sequence", result.error_message());
    EXPECT_TRUE(backend.is_connected());
    ASSERT_TRUE(backend.execute_query("SELECT 0", rs::util::Deadline::max()));
  }
}

TEST(BackendResultContractTest, SuccessSnapshotsOwnFinalStateAcrossFailureAndDestruction) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::OwnedResultCells, Mode::OwnedResultCellsTransaction, Mode::OwnedResultCellsAborted}) {
    const SessionSnapshot expected{
        mode == Mode::OwnedResultCells ? SessionState::Idle :
        mode == Mode::OwnedResultCellsTransaction ? SessionState::Transaction : SessionState::FailedTransaction,
        mode == Mode::OwnedResultCells ? SessionDisposition::Reusable : SessionDisposition::ResetRequired};
    BackendResult<QueryResult> saved{QueryResult{}};
    {
      postgres::PgDatabaseConnection backend(std::make_unique<ScriptedBackendTransport>(mode));
      ConnectionSettings settings; settings.use_ssl = false;
      ASSERT_TRUE(backend.connect(settings));
      auto result = backend.execute_query("first", rs::util::Deadline::max());
      ASSERT_TRUE(result); EXPECT_EQ(expected, result.session_snapshot());
      ASSERT_EQ(1u, result->additional_results.size());
      ASSERT_TRUE(result->additional_results[0].error);
      EXPECT_EQ(expected.state, result->additional_results[0].error->session_state);
      EXPECT_EQ(expected.disposition, result->additional_results[0].error->disposition);
      saved = result;
      auto moved = std::move(result);
      EXPECT_EQ(expected, moved.session_snapshot());
      const auto later = backend.execute_query("later", rs::util::Deadline::max());
      ASSERT_TRUE(later.has_error());
      EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), later.session_snapshot());
      EXPECT_EQ(expected, moved.session_snapshot());
      backend.disconnect();
    }
    EXPECT_EQ(expected, saved.session_snapshot());
    EXPECT_EQ(3u, saved->rows.size());
  }
}

TEST(BackendResultContractTest, PreparedAndDescriptionSuccessReportFinalIdleState) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const bool describe : {false, true}) {
    GenericDatabaseConnection backend(std::make_unique<postgres::PgProtocolParser>(),
        std::make_unique<ScriptedBackendTransport>(describe ? Mode::DescriptionOneColumn : Mode::PreparedCommand));
    ConnectionSettings settings; settings.use_ssl = false;
    ASSERT_TRUE(backend.connect(settings));
    const QueryParameter parameters[]{
        {"a", QueryParameterType::Text}, {"b", QueryParameterType::Text},
        {"c", QueryParameterType::Text}, {std::string("x"), QueryParameterType::Binary}};
    auto result = describe
        ? backend.describe_statement("SELECT value", {}, rs::util::Deadline::max())
        : backend.execute_prepared("?,?,?,?", parameters, rs::util::Deadline::max());
    ASSERT_TRUE(result) << result.error_message();
    EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), result.session_snapshot());
    backend.disconnect();
    EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), result.session_snapshot());
  }
}

TEST(BackendResultContractTest, UnreportedSuccessIsConservativeAndErrorSnapshotHasOneSource) {
  using namespace rs::core::database;
  const SessionSnapshot unknown{};
  BackendResult<QueryResult> value{QueryResult{}};
  BackendResult<void> empty;
  EXPECT_EQ(unknown, value.session_snapshot()); EXPECT_EQ(unknown, empty.session_snapshot());
  const SessionSnapshot idle{SessionState::Idle, SessionDisposition::Reusable};
  BackendResult<void> completed{idle}; EXPECT_EQ(idle, completed.session_snapshot());
  BackendError error{rs::util::make_error_code(rs::util::DbErrorCode::QueryFailed), "owned"};
  error.session_state = SessionState::Transaction; error.disposition = SessionDisposition::ResetRequired;
  BackendResult<QueryResult> failed{error}; BackendResult<void> failed_void{error};
  const SessionSnapshot transaction{SessionState::Transaction, SessionDisposition::ResetRequired};
  EXPECT_EQ(transaction, failed.session_snapshot()); EXPECT_EQ(transaction, failed_void.session_snapshot());
  // Existing internal error annotation remains authoritative, with no stale copy.
  failed.backend_error().session_state = SessionState::Disconnected;
  failed.backend_error().disposition = SessionDisposition::Retire;
  EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), failed.session_snapshot());
}

TEST(BackendResultContractTest, ConnectSnapshotSurvivesLocalReconnectRejectionAndLaterLoss) {
  using namespace rs::core::database;
  BackendResult<void> saved;
  const SessionSnapshot idle{SessionState::Idle, SessionDisposition::Reusable};
  {
    auto transport = std::make_unique<ScriptedBackendTransport>(ScriptedBackendTransport::ResponseMode::QueryReadTimeout);
    auto* observed = transport.get();
    GenericDatabaseConnection backend(std::make_unique<postgres::PgProtocolParser>(), std::move(transport));
    ConnectionSettings settings; settings.use_ssl = false;
    auto opened = backend.connect(settings);
    ASSERT_TRUE(opened); EXPECT_EQ(idle, opened.session_snapshot());
    saved = opened;
    auto moved = std::move(opened); EXPECT_EQ(idle, moved.session_snapshot());
    const auto sends = observed->send_count();
    auto invalid = settings; invalid.password = std::string("a\0b", 3);
    const auto rejected = backend.connect(invalid);
    ASSERT_TRUE(rejected.has_error()); EXPECT_EQ(idle, rejected.session_snapshot());
    EXPECT_EQ(sends, observed->send_count()); EXPECT_EQ(0u, observed->close_count());
    const auto lost = backend.execute_query("later", rs::util::Deadline::max());
    ASSERT_TRUE(lost.has_error());
    EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), lost.session_snapshot());
    EXPECT_EQ(idle, moved.session_snapshot()); backend.disconnect();
  }
  EXPECT_EQ(idle, saved.session_snapshot());
}

TEST(BackendTransactionTest, SuccessAdaptersPreserveReportedSnapshotRatherThanCurrentProbeState) {
  using namespace rs::core::database;
  TransactionProbe backend;
  const auto deadline = rs::util::Deadline::min();
  for (const auto state : {SessionState::Idle, SessionState::Transaction, SessionState::FailedTransaction, SessionState::Unknown}) {
    backend.snapshot = {state, state == SessionState::Idle ? SessionDisposition::Reusable :
        state == SessionState::Unknown ? SessionDisposition::Retire : SessionDisposition::ResetRequired};
    for (const auto action : {TransactionAction::Begin, TransactionAction::Commit, TransactionAction::Rollback}) {
      const auto result = backend.transaction(action, deadline);
      ASSERT_TRUE(result); EXPECT_EQ(backend.snapshot, result.session_snapshot());
      EXPECT_EQ(deadline, backend.observed_deadline);
    }
    for (const auto level : transaction_isolations) {
      const auto result = backend.set_transaction_isolation(level, deadline);
      ASSERT_TRUE(result); EXPECT_EQ(backend.snapshot, result.session_snapshot());
      EXPECT_EQ(deadline, backend.observed_deadline);
    }
    EXPECT_EQ(SessionState::Disconnected, backend.session_state());
  }
}

TEST(BackendTransactionTest, NativeSuccessSnapshotsTrackBeginCommitRollbackAndIsolation) {
  using namespace rs::core::database;
  postgres::PgDatabaseConnection backend(std::make_unique<ScriptedBackendTransport>(
      ScriptedBackendTransport::ResponseMode::TransactionCompletions));
  ConnectionSettings settings; settings.use_ssl = false;
  ASSERT_TRUE(backend.connect(settings));
  const auto deadline = rs::util::Deadline::max();
  const auto begun = backend.transaction(TransactionAction::Begin, deadline);
  ASSERT_TRUE(begun);
  EXPECT_EQ((SessionSnapshot{SessionState::Transaction, SessionDisposition::ResetRequired}), begun.session_snapshot());
  const auto committed = backend.transaction(TransactionAction::Commit, deadline);
  ASSERT_TRUE(committed);
  const SessionSnapshot idle{SessionState::Idle, SessionDisposition::Reusable};
  EXPECT_EQ(idle, committed.session_snapshot());
  const auto rolled_back = backend.transaction(TransactionAction::Rollback, deadline);
  ASSERT_TRUE(rolled_back); EXPECT_EQ(idle, rolled_back.session_snapshot());
  const auto isolation = backend.set_transaction_isolation(TransactionIsolation::Serializable, deadline);
  ASSERT_TRUE(isolation); EXPECT_EQ(idle, isolation.session_snapshot());
  backend.disconnect();
  EXPECT_EQ((SessionSnapshot{SessionState::Transaction, SessionDisposition::ResetRequired}), begun.session_snapshot());
  EXPECT_EQ(idle, committed.session_snapshot()); EXPECT_EQ(idle, isolation.session_snapshot());
}

TEST(BackendTransactionTest, OptionalFacetIsStableAcrossConnectionAndDisconnect) {
  using namespace rs::core::database;
  postgres::PgDatabaseConnection backend(std::make_unique<ScriptedBackendTransport>(
      ScriptedBackendTransport::ResponseMode::TransactionCompletions));
  IDatabaseConnection& session = backend;
  auto* facet = session.transaction_session(); ASSERT_NE(nullptr, facet);
  EXPECT_TRUE(facet->transaction_capabilities().supported);
  const auto closed = facet->transaction(TransactionAction::Begin, rs::util::Deadline::max());
  ASSERT_TRUE(closed.has_error());
  EXPECT_EQ(BackendErrorClass::NotConnected, closed.backend_error().error_class);
  EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), closed.session_snapshot());
  ConnectionSettings settings; settings.use_ssl = false;
  ASSERT_TRUE(session.connect(settings)); EXPECT_EQ(facet, session.transaction_session());
  const auto begun = facet->transaction(TransactionAction::Begin, rs::util::Deadline::max());
  ASSERT_TRUE(begun);
  EXPECT_EQ((SessionSnapshot{SessionState::Transaction, SessionDisposition::ResetRequired}), begun.session_snapshot());
  session.disconnect(); EXPECT_EQ(facet, session.transaction_session());
  const auto disconnected = facet->set_transaction_isolation(TransactionIsolation::Serializable, rs::util::Deadline::max());
  ASSERT_TRUE(disconnected.has_error());
  EXPECT_EQ(BackendErrorClass::NotConnected, disconnected.backend_error().error_class);
  EXPECT_EQ(BackendOperation::SetTransactionIsolation, disconnected.backend_error().operation);
}

TEST(NormalizedParameterTest, DescriptionFacetIsStableAndOwnsMetadataAcrossSessionLifetime) {
  using namespace rs::core::database;
  BackendResult<QueryResult> retained{QueryResult{}};
  {
    ParameterNormalizationConnection backend;
    IDatabaseConnection& session = backend;
    auto* facet = session.statement_description(); ASSERT_NE(nullptr, facet);
    const QueryParameterType hints[]{QueryParameterType::Numeric};
    const auto closed = facet->describe_statement("SELECT ?", hints, rs::util::Deadline::max());
    ASSERT_TRUE(closed.has_error()); EXPECT_EQ(BackendErrorClass::NotConnected, closed.backend_error().error_class);
    EXPECT_EQ(BackendOperation::Describe, closed.backend_error().operation);
    ConnectionSettings settings; settings.use_ssl = false;
    ASSERT_TRUE(session.connect(settings)); EXPECT_EQ(facet, session.statement_description());
    const auto deadline = rs::util::make_deadline(std::chrono::seconds(2));
    retained = facet->describe_statement("SELECT ?", hints, deadline);
    ASSERT_TRUE(retained); EXPECT_EQ(deadline, backend.observed);
    EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), retained.session_snapshot());
    EXPECT_TRUE(retained->rows.empty()); EXPECT_TRUE(retained->additional_results.empty());
    session.disconnect(); EXPECT_EQ(facet, session.statement_description());
    const auto disconnected = facet->describe_statement("SELECT ?", hints, deadline);
    ASSERT_TRUE(disconnected.has_error());
    EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), disconnected.session_snapshot());
  }
  ASSERT_EQ(1u, retained->normalized_parameter_types.size());
  EXPECT_EQ(ScalarType::Numeric, retained->normalized_parameter_types[0].type);
  EXPECT_EQ(18u, retained->normalized_parameter_types[0].column_size);
  EXPECT_EQ((SessionSnapshot{SessionState::Idle, SessionDisposition::Reusable}), retained.session_snapshot());
}

TEST(CatalogQueryTest, OptionalCatalogFacetIsStableAndQueryOwnsBorrowedFilters) {
  using namespace rs::core::database;
  std::string retained;
  {
    postgres::PgDatabaseConnection backend(std::make_unique<ScriptedBackendTransport>(
        ScriptedBackendTransport::ResponseMode::TransactionCompletions));
    const IDatabaseConnection& session = backend;
    const auto* facet = session.catalog_queries(); ASSERT_NE(nullptr, facet);
    TablesCatalogRequest request; request.schema = "s'chema"; request.table = "t\\_%";
    auto query = facet->catalog_query(request); ASSERT_TRUE(query);
    retained = std::move(*query); request.schema = "overwritten"; request.table.reset();
    EXPECT_NE(std::string::npos, retained.find("s''chema"));
    EXPECT_EQ(std::string::npos, retained.find("overwritten"));
    EXPECT_FALSE(session.is_connected()); EXPECT_EQ(SessionState::Disconnected, session.session_state());
    ConnectionSettings settings; settings.use_ssl = false;
    ASSERT_TRUE(backend.connect(settings)); EXPECT_EQ(facet, session.catalog_queries());
    auto live = facet->catalog_query(ColumnsCatalogRequest{}); ASSERT_TRUE(live);
    EXPECT_EQ(SessionState::Idle, session.session_state());
    backend.disconnect(); EXPECT_EQ(facet, session.catalog_queries());
    auto closed = facet->catalog_query(ColumnsCatalogRequest{}); ASSERT_TRUE(closed);
    EXPECT_EQ(*live, *closed);
  }
  EXPECT_NE(std::string::npos, retained.find("s''chema"));
  EXPECT_NE(std::string::npos, retained.find("table_name LIKE"));
}

TEST(SessionHealthTest, NativeTransportInterruptionRetiresWithHealthContext) {
  using namespace rs::core::database;
  using Mode = ScriptedBackendTransport::ResponseMode;
  for (const auto mode : {Mode::QueryReadTimeout, Mode::PartialQueryWrite, Mode::UnknownQueryFrame}) {
    postgres::PgDatabaseConnection session(std::make_unique<ScriptedBackendTransport>(mode));
    ConnectionSettings settings; settings.use_ssl = false;
    ASSERT_TRUE(session.connect(settings));
    auto* health = session.session_health();
    const auto result = health->check_health(rs::util::make_deadline(std::chrono::seconds(1)));
    ASSERT_FALSE(result);
    EXPECT_EQ(BackendOperation::CheckHealth, result.backend_error().operation);
    EXPECT_EQ((SessionSnapshot{SessionState::Disconnected, SessionDisposition::Retire}), result.session_snapshot());
    EXPECT_FALSE(session.is_connected());
    EXPECT_EQ(health, session.session_health());
  }
}

TEST(SessionHealthTest, GenericFamilyMachineryDoesNotAdvertiseActiveProbe) {
  rs::core::database::GenericDatabaseConnection session(
      std::make_unique<rs::core::database::postgres::PgProtocolParser>());
  EXPECT_EQ(nullptr, session.session_health());
}
