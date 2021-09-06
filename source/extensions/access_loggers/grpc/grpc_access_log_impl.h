#pragma once

#include <absl/container/flat_hash_map.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/descriptor.pb.h>

#include <chrono>
#include <memory>

#include "envoy/common/time.h"
#include "envoy/data/accesslog/v3/accesslog.pb.h"
#include "envoy/event/dispatcher.h"
#include "envoy/extensions/access_loggers/grpc/v3/als.pb.h"
#include "envoy/grpc/async_client_manager.h"
#include "envoy/local_info/local_info.h"
#include "envoy/service/accesslog/v3/als.pb.h"
#include "envoy/stats/stats_macros.h"
#include "envoy/thread_local/thread_local.h"

#include "source/common/common/linked_object.h"
#include "source/common/grpc/buffered_async_client_impl.h"
#include "source/extensions/access_loggers/common/grpc_access_logger.h"

namespace Envoy {
namespace Extensions {
namespace AccessLoggers {
namespace GrpcCommon {

static constexpr absl::string_view GRPC_LOG_STATS_PREFIX = "access_logs.grpc_access_log.";

#define CRITICAL_ACCESS_LOGGER_GRPC_CLIENT_STATS(COUNTER, GAUGE)                                   \
  COUNTER(critical_logs_message_timeout)                                                           \
  COUNTER(critical_logs_nack_received)                                                             \
  COUNTER(critical_logs_ack_received)                                                              \
  GAUGE(pending_critical_logs, Accumulate)

struct CriticalAccessLoggerGrpcClientStats {
  CRITICAL_ACCESS_LOGGER_GRPC_CLIENT_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT)
};

class CriticalAccessLogger {
public:
  using RequestType = envoy::service::accesslog::v3::CriticalAccessLogsMessage;
  using ResponseType = envoy::service::accesslog::v3::CriticalAccessLogsResponse;

  struct CriticalLogStream : public Grpc::AsyncStreamCallbacks<ResponseType> {
    explicit CriticalLogStream(CriticalAccessLogger& parent) : parent_(parent) {}

    // Grpc::AsyncStreamCallbacks
    void onCreateInitialMetadata(Http::RequestHeaderMap&) override {}
    void onReceiveInitialMetadata(Http::ResponseHeaderMapPtr&&) override {}
    void onReceiveMessage(std::unique_ptr<ResponseType>&& message) override {
      const auto& id = message->id();

      switch (message->status()) {
      case envoy::service::accesslog::v3::CriticalAccessLogsResponse::ACK:
        parent_.inflight_message_ttl_->received(id);
        parent_.stats_.critical_logs_ack_received_.inc();
        parent_.stats_.pending_critical_logs_.dec();
        parent_.client_->clearPendingMessage(id);
        break;
      case envoy::service::accesslog::v3::CriticalAccessLogsResponse::NACK:
        parent_.stats_.critical_logs_nack_received_.inc();
        parent_.client_->bufferMessage(id);
        break;
      default:
        return;
      }
    }
    void onReceiveTrailingMetadata(Http::ResponseTrailerMapPtr&&) override {}
    void onRemoteClose(Grpc::Status::GrpcStatus, const std::string&) override {
      parent_.client_->cleanup();
    }

    CriticalAccessLogger& parent_;
  };

  class InflightMessageTtlManager {
  public:
    InflightMessageTtlManager(Event::Dispatcher& dispatcher,
                              CriticalAccessLoggerGrpcClientStats& stats,
                              Grpc::BufferedAsyncClient<RequestType, ResponseType>& client,
                              std::chrono::milliseconds message_ack_timeout)
        : dispatcher_(dispatcher), message_ack_timeout_(message_ack_timeout) {
      timer_ = dispatcher_.createTimer([this, &client, &stats] {
        const auto now = dispatcher_.timeSource().monotonicTime();

        std::cout << "deadline expired" << std::endl;
        auto it = deadline_.lower_bound(now);
        while (it != deadline_.end()) {
          for (auto&& id : it->second) {
            if (received_ids_.find(id) != received_ids_.end()) {
              received_ids_.erase(id);
              continue;
            }

            client.bufferMessage(id);
            stats.critical_logs_message_timeout_.inc();
          }
          ++it;
        }
        timer_->enableTimer(message_ack_timeout_);
      });

      timer_->enableTimer(message_ack_timeout_);
    }

    void timeToString(std::chrono::steady_clock::time_point t)
    {
      auto microsecondsUTC = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();

        std::cout << "It took me " << microsecondsUTC << " seconds." << std::endl;
    }

    ~InflightMessageTtlManager() { timer_->disableTimer(); }

    void setDeadline(std::set<uint32_t>&& ids) {
      auto expires_at = dispatcher_.timeSource().monotonicTime() + message_ack_timeout_;
      deadline_.emplace(expires_at, std::move(ids));
    }

    void received(uint32_t id) { received_ids_.emplace(id); }

  private:
    Event::Dispatcher& dispatcher_;
    std::chrono::milliseconds message_ack_timeout_;
    Event::TimerPtr timer_;
    std::map<MonotonicTime, std::set<uint32_t>, std::greater<>> deadline_;
    std::set<uint32_t> received_ids_;
  };

  CriticalAccessLogger(const Grpc::RawAsyncClientSharedPtr& client,
                       const Protobuf::MethodDescriptor& method, Event::Dispatcher& dispatcher,
                       Stats::Scope& scope, uint64_t message_ack_timeout,
                       uint64_t max_pending_buffer_size_bytes);

  void flush(RequestType& message);

  bool shouldSetLogIdentifier() { return client_->hasActiveStream(); }

private:
  friend CriticalLogStream;

  Event::Dispatcher& dispatcher_;
  std::chrono::milliseconds message_ack_timeout_;
  CriticalAccessLoggerGrpcClientStats stats_;
  CriticalLogStream stream_callback_;
  Grpc::BufferedAsyncClientPtr<RequestType, ResponseType> client_;
  std::unique_ptr<InflightMessageTtlManager> inflight_message_ttl_;
};

class GrpcAccessLoggerImpl
    : public Common::GrpcAccessLogger<envoy::data::accesslog::v3::HTTPAccessLogEntry,
                                      envoy::data::accesslog::v3::TCPAccessLogEntry,
                                      envoy::service::accesslog::v3::StreamAccessLogsMessage,
                                      envoy::service::accesslog::v3::StreamAccessLogsResponse> {
public:
  GrpcAccessLoggerImpl(
      const Grpc::RawAsyncClientSharedPtr& client,
      const envoy::extensions::access_loggers::grpc::v3::CommonGrpcAccessLogConfig& config,
      std::chrono::milliseconds buffer_flush_interval_msec, uint64_t max_buffer_size_bytes,
      Event::Dispatcher& dispatcher, const LocalInfo::LocalInfo& local_info, Stats::Scope& scope);

private:
  bool isCriticalMessageEmpty();
  void initCriticalMessage();
  void addCriticalMessageEntry(envoy::data::accesslog::v3::HTTPAccessLogEntry&& entry);
  void addCriticalMessageEntry(envoy::data::accesslog::v3::TCPAccessLogEntry&& entry);
  void clearCriticalMessage() { critical_message_.Clear(); }

  // Extensions::AccessLoggers::GrpcCommon::GrpcAccessLogger
  void addEntry(envoy::data::accesslog::v3::HTTPAccessLogEntry&& entry) override;
  void addEntry(envoy::data::accesslog::v3::TCPAccessLogEntry&& entry) override;
  bool isEmpty() override;
  void initMessage() override;
  void flushCriticalMessage() override;
  void logCritical(envoy::data::accesslog::v3::HTTPAccessLogEntry&&) override;

  uint64_t approximate_critical_message_size_bytes_ = 0;
  uint64_t max_critical_message_size_bytes_ = 0;
  std::unique_ptr<CriticalAccessLogger> critical_logger_;
  envoy::service::accesslog::v3::CriticalAccessLogsMessage critical_message_;
  const std::string log_name_;
  const LocalInfo::LocalInfo& local_info_;
};

class GrpcAccessLoggerCacheImpl
    : public Common::GrpcAccessLoggerCache<
          GrpcAccessLoggerImpl,
          envoy::extensions::access_loggers::grpc::v3::CommonGrpcAccessLogConfig> {
public:
  GrpcAccessLoggerCacheImpl(Grpc::AsyncClientManager& async_client_manager, Stats::Scope& scope,
                            ThreadLocal::SlotAllocator& tls,
                            const LocalInfo::LocalInfo& local_info);

private:
  // Common::GrpcAccessLoggerCache
  GrpcAccessLoggerImpl::SharedPtr
  createLogger(const envoy::extensions::access_loggers::grpc::v3::CommonGrpcAccessLogConfig& config,
               const Grpc::RawAsyncClientSharedPtr& client,
               std::chrono::milliseconds buffer_flush_interval_msec, uint64_t max_buffer_size_bytes,
               Event::Dispatcher& dispatcher, Stats::Scope& scope) override;

  const LocalInfo::LocalInfo& local_info_;
};

/**
 * Aliases for class interfaces for mock definitions.
 */
using GrpcAccessLogger = GrpcAccessLoggerImpl::Interface;
using GrpcAccessLoggerSharedPtr = GrpcAccessLogger::SharedPtr;

using GrpcAccessLoggerCache = GrpcAccessLoggerCacheImpl::Interface;
using GrpcAccessLoggerCacheSharedPtr = GrpcAccessLoggerCache::SharedPtr;

} // namespace GrpcCommon
} // namespace AccessLoggers
} // namespace Extensions
} // namespace Envoy
