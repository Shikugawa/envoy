#include "source/extensions/access_loggers/grpc/grpc_access_log_impl.h"

#include <chrono>

#include "envoy/data/accesslog/v3/accesslog.pb.h"
#include "envoy/extensions/access_loggers/grpc/v3/als.pb.h"
#include "envoy/grpc/async_client_manager.h"
#include "envoy/local_info/local_info.h"

#include "source/common/config/utility.h"
#include "source/common/grpc/typed_async_client.h"

namespace Envoy {
namespace Extensions {
namespace AccessLoggers {
namespace GrpcCommon {

GrpcAccessLoggerImpl::GrpcAccessLoggerImpl(
    const Grpc::RawAsyncClientSharedPtr& client,
    const envoy::extensions::access_loggers::grpc::v3::CommonGrpcAccessLogConfig& config,
    std::chrono::milliseconds buffer_flush_interval_msec, uint64_t max_buffer_size_bytes,
    Event::Dispatcher& dispatcher, const LocalInfo::LocalInfo& local_info, Stats::Scope& scope)
    : GrpcAccessLogger(std::move(client), buffer_flush_interval_msec, max_buffer_size_bytes,
                       dispatcher, scope, GRPC_LOG_STATS_PREFIX.data(),
                       *Protobuf::DescriptorPool::generated_pool()->FindMethodByName(
                           "envoy.service.accesslog.v3.AccessLogService.StreamAccessLogs")),
      approximate_critical_message_size_bytes_(max_buffer_size_bytes), log_name_(config.log_name()),
      local_info_(local_info) {
  critical_client_ = std::make_unique<CriticalAccessLoggerGrpcClientImpl<
      envoy::service::accesslog::v3::BufferedCriticalAccessLogsMessage>>(
      client,
      *Protobuf::DescriptorPool::generated_pool()->FindMethodByName(
          "envoy.service.accesslog.v3.AccessLogService.BufferedCriticalAccessLogs"),
      dispatcher, scope, PROTOBUF_GET_MS_OR_DEFAULT(config, message_ack_timeout, 5000),
      PROTOBUF_GET_WRAPPED_OR_DEFAULT(config, pending_critical_buffer_size_bytes, 16384));
}

void GrpcAccessLoggerImpl::addEntry(envoy::data::accesslog::v3::HTTPAccessLogEntry&& entry) {
  message_.mutable_http_logs()->mutable_log_entry()->Add(std::move(entry));
}

void GrpcAccessLoggerImpl::addEntry(envoy::data::accesslog::v3::TCPAccessLogEntry&& entry) {
  message_.mutable_tcp_logs()->mutable_log_entry()->Add(std::move(entry));
}

void GrpcAccessLoggerImpl::addCriticalMessageEntry(
    envoy::data::accesslog::v3::HTTPAccessLogEntry&& entry) {
  critical_message_.mutable_message()->mutable_http_logs()->mutable_log_entry()->Add(
      std::move(entry));
}

void GrpcAccessLoggerImpl::addCriticalMessageEntry(
    envoy::data::accesslog::v3::TCPAccessLogEntry&& entry) {
  critical_message_.mutable_message()->mutable_tcp_logs()->mutable_log_entry()->Add(
      std::move(entry));
}

bool GrpcAccessLoggerImpl::isEmpty() {
  return !message_.has_http_logs() && !message_.has_tcp_logs();
}

bool GrpcAccessLoggerImpl::isCriticalMessageEmpty() {
  return !critical_message_.message().has_http_logs() &&
         !critical_message_.message().has_tcp_logs();
}

void GrpcAccessLoggerImpl::flushCriticalMessage() {
  if (critical_client_ == nullptr || isCriticalMessageEmpty()) {
    return;
  }
  if (!critical_client_->isStreamStarted()) {
    initCriticalMessage();
  }

  approximate_critical_message_size_bytes_ = 0;
  critical_client_->flush(critical_message_);
  clearCriticalMessage();
}

void GrpcAccessLoggerImpl::logCritical(envoy::data::accesslog::v3::HTTPAccessLogEntry&& entry) {
  approximate_critical_message_size_bytes_ += entry.ByteSizeLong();
  addCriticalMessageEntry(std::move(entry));

  if (approximate_critical_message_size_bytes_ >= max_critical_buffer_size_bytes_) {
    flushCriticalMessage();
  }
}

void GrpcAccessLoggerImpl::initMessage() {
  auto* identifier = message_.mutable_identifier();
  *identifier->mutable_node() = local_info_.node();
  identifier->set_log_name(log_name_);
}

void GrpcAccessLoggerImpl::initCriticalMessage() {
  auto* identifier = critical_message_.mutable_message()->mutable_identifier();
  *identifier->mutable_node() = local_info_.node();
  identifier->set_log_name(log_name_);
}

GrpcAccessLoggerCacheImpl::GrpcAccessLoggerCacheImpl(Grpc::AsyncClientManager& async_client_manager,
                                                     Stats::Scope& scope,
                                                     ThreadLocal::SlotAllocator& tls,
                                                     const LocalInfo::LocalInfo& local_info)
    : GrpcAccessLoggerCache(async_client_manager, scope, tls), local_info_(local_info) {}

GrpcAccessLoggerImpl::SharedPtr GrpcAccessLoggerCacheImpl::createLogger(
    const envoy::extensions::access_loggers::grpc::v3::CommonGrpcAccessLogConfig& config,
    const Grpc::RawAsyncClientSharedPtr& client,
    std::chrono::milliseconds buffer_flush_interval_msec, uint64_t max_buffer_size_bytes,
    Event::Dispatcher& dispatcher, Stats::Scope& scope) {
  return std::make_shared<GrpcAccessLoggerImpl>(client, config, buffer_flush_interval_msec,
                                                max_buffer_size_bytes, dispatcher, local_info_,
                                                scope);
}

} // namespace GrpcCommon
} // namespace AccessLoggers
} // namespace Extensions
} // namespace Envoy
