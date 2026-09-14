// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - WebSocket Endpoint Handlers
//
// Manages WebSocket connections for real-time streaming:
//   /v1/ws/stats  — Stats snapshot + periodic deltas
//   /v1/ws/events — Batched event stream
//   /v1/ws/logs   — Real-time log streaming with ring buffer
//
// Runs entirely on the libuv event loop.  Thread-safe event
// posting via PostEvent() and PostLog().

#ifndef PAGESPEED_SRC_WORKER_WS_HANDLERS_H_
#define PAGESPEED_SRC_WORKER_WS_HANDLERS_H_

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"
#include "src/worker/websocket.h"
#include "uv.h"

namespace pagespeed {

class MessageHandler;

// Configuration for WebSocket endpoints.
struct WsConfig {
  int max_connections = 8;       // Max simultaneous WS connections.
  int ping_interval_ms = 30000;  // Server ping interval.
  int pong_timeout_ms = 10000;   // Close if pong not received.
  int auth_timeout_ms = 2000;    // Close if auth not received.
  int stats_interval_ms = 1000;  // Stats push interval (default).
  int stats_min_interval_ms = 100;
  int stats_max_interval_ms = 60000;
  int event_batch_ms = 100;  // Event batch interval.
  size_t event_hwm = 65536;  // Per-client event buffer high-water.
  std::string auth_token;    // Empty = no credential configured.
  // An empty auth_token is only "no auth required" when the
  // operator deliberately opted out (--api-no-auth).  Otherwise a tokenless
  // server fails closed, here as in HttpServer::CheckAuth.
  bool allow_unauthenticated = false;
  bool read_open =
      false;  // Skip WS auth when read_open (streams are read-only).
};

// Metrics for WebSocket subsystem.
struct WsMetrics {
  std::atomic<int> stats_connections{0};
  std::atomic<int> events_connections{0};
  std::atomic<int> logs_connections{0};
  std::atomic<uint64_t> messages_sent{0};
  std::atomic<uint64_t> messages_dropped{0};
};

// Forward declaration — connection state is internal.
struct WsConnection;

// WebSocket endpoint manager.
//
// Usage:
//   WsManager mgr(loop, config, handler);
//   mgr.SetStatsProvider([&]() { return BuildStatsJson(); });
//   mgr.Start();
//   // From any thread:
//   mgr.PostEvent("variant_written", detail_json);
//   // On shutdown:
//   mgr.Stop();
class WsManager {
 public:
  WsManager(uv_loop_t* loop, WsConfig config, MessageHandler* handler);
  ~WsManager();

  WsManager(const WsManager&) = delete;
  WsManager& operator=(const WsManager&) = delete;

  // Set the callback that produces the full stats JSON snapshot.
  // Called on the event loop thread during stats push.
  void SetStatsProvider(std::function<nlohmann::json()> provider);

  // Start timers (ping, stats, event batch).
  void Start();

  // Stop all timers and close all connections.
  void Stop();

  // Attempt to upgrade an HTTP connection to WebSocket.
  // Called from the HTTP server's OnMessageComplete when the path
  // matches /v1/ws/stats, /v1/ws/events, or /v1/ws/logs.
  // Takes ownership of the handle, which the HTTP server allocated as a
  // uv_any_handle (a TCP socket or, under --api-socket, a unix pipe) and
  // hands over as a uv_stream_t*.  It must be freed as a uv_any_handle*.
  // `endpoint` is "stats", "events", or "logs".
  // `client_key` is the Sec-WebSocket-Key header value.
  // `interval_ms` is the client-requested stats interval (0 = default).
  void AcceptUpgrade(uv_stream_t* handle, std::string_view endpoint,
                     std::string_view client_key, int interval_ms = 0);

  // Post an event from any thread.  Thread-safe.
  // `type` is the event name (e.g., "variant_written").
  // `detail` is the event payload JSON object.
  void PostEvent(std::string_view type, nlohmann::json detail);

  // Post a log entry from any thread.  Thread-safe.
  // `level` is "debug", "info", "warning", or "error".
  // `source` is the originating subsystem (e.g., "worker").
  // `module` is a finer-grained tag (e.g., "image", "html").
  // `message` is the formatted log line.
  void PostLog(std::string_view level, std::string_view source,
               std::string_view module, std::string message);

  // Access metrics.
  const WsMetrics& metrics() const { return metrics_; }

  // Total active WebSocket connections.
  int active_connections() const;

 private:
  // Timer callbacks.
  static void OnPingTimer(uv_timer_t* timer);
  static void OnStatsTimer(uv_timer_t* timer);
  static void OnEventBatchTimer(uv_timer_t* timer);

  // libuv callbacks for WS connections.
  static void OnAlloc(uv_handle_t* handle, size_t suggested_size,
                      uv_buf_t* buf);
  static void OnRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
  static void OnWriteDone(uv_write_t* req, int status);
  static void OnClose(uv_handle_t* handle);
  static void OnAuthTimeout(uv_timer_t* timer);
  static void OnPongTimeout(uv_timer_t* timer);

  // Async callbacks for cross-thread posting.
  static void OnAsyncEvent(uv_async_t* async);
  static void OnAsyncLog(uv_async_t* async);

  // Log batch timer callback.
  static void OnLogBatchTimer(uv_timer_t* timer);

  // Internal helpers.
  void HandleFrame(WsConnection* conn, const WsFrame& frame);
  void HandleAuthMessage(WsConnection* conn, std::string_view payload);
  void SendText(WsConnection* conn, std::string_view text);
  void SendFrame(WsConnection* conn, const std::string& frame_data);
  void CloseWs(WsConnection* conn, uint16_t code);
  void RemoveConnection(WsConnection* conn);
  void PushStats();
  void FlushEventBatch();
  void DrainPendingEvents();
  void DrainPendingLogs();
  void FlushLogBatch();
  void SendLogSnapshot(WsConnection* conn);

  uv_loop_t* loop_;
  WsConfig config_;
  MessageHandler* handler_;
  WsMetrics metrics_;
  std::atomic<bool> running_ = false;

  // Stats provider callback.
  std::function<nlohmann::json()> stats_provider_;

  // Previous stats snapshot for delta computation.
  nlohmann::json prev_stats_;
  uint64_t stats_seq_ = 0;
  uint64_t events_seq_ = 0;

  // Timers.
  uv_timer_t ping_timer_;
  uv_timer_t stats_timer_;
  uv_timer_t event_batch_timer_;

  // Cross-thread event posting.
  uv_async_t async_event_;
  std::mutex event_mutex_;
  std::vector<nlohmann::json> pending_events_;

  // Cross-thread log posting.
  uv_async_t async_log_;
  uv_timer_t log_batch_timer_;
  std::mutex log_mutex_;
  std::vector<nlohmann::json> pending_logs_;
  std::deque<nlohmann::json> log_ring_;
  uint64_t log_total_ = 0;  // Total logs ever posted (for overflow count).
  static constexpr size_t kMaxLogRingSize = 2000;
  static constexpr size_t kMaxPendingQueueSize = 10000;

  // Active connections (vector mutated on loop thread only).
  std::vector<WsConnection*> connections_;
  // Atomic mirror of connections_.size() for thread-safe reads.
  std::atomic<int> connection_count_{0};
};

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_WS_HANDLERS_H_
