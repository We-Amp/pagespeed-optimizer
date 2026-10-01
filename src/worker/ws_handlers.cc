// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - WebSocket Endpoint Handlers Implementation

#include "src/worker/ws_handlers.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <string>
#include <utility>

#include "lib/base/message_handler.h"
#include "lib/base/string_util.h"
#include "src/worker/uv_helpers.h"
#include "src/worker/websocket.h"

namespace pagespeed {

using json = nlohmann::json;

namespace {

constexpr std::string_view kReplacementCharacter = "\xEF\xBF\xBD";  // U+FFFD
constexpr std::string_view kEllipsis = "\xE2\x80\xA6";              // U+2026

// Length of the well-formed UTF-8 sequence that starts at s[i] (RFC 3629;
// Unicode's table of well-formed byte sequences), or 0 when none does:
// a stray continuation byte, an overlong lead (C0, C1), F5..FF, a surrogate
// (ED A0..BF), anything above U+10FFFF (F4 90..), or a sequence the end of
// the string cuts short.
size_t Utf8SequenceLength(std::string_view s, size_t i) {
  const auto byte = [&s](size_t k) { return static_cast<unsigned char>(s[k]); };
  const unsigned char lead = byte(i);
  if (lead < 0x80) return 1;
  size_t length = 0;
  unsigned char second_lo = 0x80;
  unsigned char second_hi = 0xBF;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
  } else if (lead == 0xE0) {
    length = 3;
    second_lo = 0xA0;  // no overlong three-byte forms
  } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
    length = 3;
  } else if (lead == 0xED) {
    length = 3;
    second_hi = 0x9F;  // no UTF-16 surrogates
  } else if (lead == 0xF0) {
    length = 4;
    second_lo = 0x90;  // no overlong four-byte forms
  } else if (lead >= 0xF1 && lead <= 0xF3) {
    length = 4;
  } else if (lead == 0xF4) {
    length = 4;
    second_hi = 0x8F;  // nothing above U+10FFFF
  } else {
    return 0;
  }
  if (i + length > s.size()) return 0;
  if (byte(i + 1) < second_lo || byte(i + 1) > second_hi) return 0;
  for (size_t k = 2; k < length; ++k) {
    if (byte(i + k) < 0x80 || byte(i + k) > 0xBF) return 0;
  }
  return length;
}

// Every serialization of a log entry goes through here: the replace handler
// turns any invalid UTF-8 into U+FFFD instead of throwing on the loop thread.
// SanitizeLogMessage already guarantees valid messages; this is the backstop.
std::string DumpLog(const json& j) {
  return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// Serialized bytes reserved for a /v1/logs envelope around its entries: the
// seven fixed keys with 20-digit numbers and the 16-digit stream id need
// about 230.
constexpr size_t kLogsPageEnvelopeReserve = 256;

// 64 random bits as 16 lowercase hex digits (two 32-bit draws).
std::string NewLogStreamId() {
  std::random_device random;
  uint64_t bits =
      (static_cast<uint64_t>(random()) << 32) | static_cast<uint64_t>(random());
  static constexpr char kHex[] = "0123456789abcdef";
  std::string id(16, '0');
  for (size_t i = id.size(); i-- > 0;) {
    id[i] = kHex[bits & 0xF];
    bits >>= 4;
  }
  return id;
}

}  // namespace

std::string SanitizeLogMessage(std::string message) {
  // Repair: copy only from the first invalid byte on (valid text, the
  // common case, is not copied).
  size_t i = 0;
  while (i < message.size()) {
    const size_t n = Utf8SequenceLength(message, i);
    if (n == 0) break;
    i += n;
  }
  if (i < message.size()) {
    std::string repaired = message.substr(0, i);
    repaired.reserve(message.size() + 8);
    while (i < message.size()) {
      const size_t n = Utf8SequenceLength(message, i);
      if (n == 0) {
        repaired.append(kReplacementCharacter);
        ++i;
      } else {
        repaired.append(message, i, n);
        i += n;
      }
    }
    message = std::move(repaired);
  }

  // Cap: the text is valid UTF-8 now, so backing up over continuation bytes
  // finds the character boundary at or below the cap.
  if (message.size() > kMaxLogMessageBytes) {
    size_t keep = kMaxLogMessageBytes;
    while (keep > 0 &&
           (static_cast<unsigned char>(message[keep]) & 0xC0) == 0x80) {
      --keep;
    }
    const size_t removed = message.size() - keep;
    message.resize(keep);
    message.append(kEllipsis);
    message.append("[truncated ");
    message.append(std::to_string(removed));
    message.append(" bytes]");
  }
  return message;
}

// ---------------------------------------------------------------------------
// WsConnection — per-client WebSocket state
// ---------------------------------------------------------------------------

struct WsConnection {
  // Owned; allocated by HttpServer as a uv_any_handle and freed as one (the
  // union arms differ in size, so the free must not name an arm).
  uv_stream_t* handle = nullptr;
  uv_timer_t auth_timer;
  uv_timer_t pong_timer;
  WsManager* manager = nullptr;
  WsFrameParser parser;
  bool closing = false;
  bool timers_initialized = false;
  bool authenticated = false;
  bool auth_required = false;
  std::string endpoint;  // "stats", "events", or "logs"
  int stats_interval_ms = 0;

  // Per-client event buffer (events endpoint only).
  std::string event_buffer;
  size_t event_buffer_bytes = 0;

  // Per-client log buffer (logs endpoint only).
  std::vector<json> log_buffer;
  size_t log_buffer_bytes = 0;

  // Write backpressure: track pending writes to prevent memory exhaustion
  // from slow-reading clients.
  int pending_writes = 0;
  static constexpr int kMaxPendingWrites = 64;

  // Outstanding uv_close callbacks.  Only call DeleteWsConnection
  // when this reaches zero (prevents cascade races during shutdown).
  int pending_closes = 0;
};

// Write context for async writes.
struct WsWriteContext {
  uv_write_t req;
  uv_buf_t buf;
  WsConnection* conn;
  std::string data;
};

// ---------------------------------------------------------------------------
// WsManager
// ---------------------------------------------------------------------------

WsManager::WsManager(uv_loop_t* loop, WsConfig config, MessageHandler* handler)
    : loop_(loop),
      config_(std::move(config)),
      handler_(handler),
      stream_id_(NewLogStreamId()) {}

WsManager::~WsManager() {
  if (running_) Stop();
}

void WsManager::SetStatsProvider(std::function<json()> provider) {
  stats_provider_ = std::move(provider);
}

void WsManager::Start() {
  if (running_) return;
  running_ = true;

  // Initialize timers.
  uv_timer_init(loop_, &ping_timer_);
  ping_timer_.data = this;
  uv_timer_start(&ping_timer_, OnPingTimer, config_.ping_interval_ms,
                 config_.ping_interval_ms);

  uv_timer_init(loop_, &stats_timer_);
  stats_timer_.data = this;
  uv_timer_start(&stats_timer_, OnStatsTimer, config_.stats_interval_ms,
                 config_.stats_interval_ms);

  uv_timer_init(loop_, &event_batch_timer_);
  event_batch_timer_.data = this;
  uv_timer_start(&event_batch_timer_, OnEventBatchTimer, config_.event_batch_ms,
                 config_.event_batch_ms);

  // Initialize async handle for cross-thread events.
  uv_async_init(loop_, &async_event_, OnAsyncEvent);
  async_event_.data = this;

  // Initialize async handle and batch timer for cross-thread logs.
  uv_async_init(loop_, &async_log_, OnAsyncLog);
  async_log_.data = this;

  uv_timer_init(loop_, &log_batch_timer_);
  log_batch_timer_.data = this;
  uv_timer_start(&log_batch_timer_, OnLogBatchTimer, config_.event_batch_ms,
                 config_.event_batch_ms);
}

void WsManager::Stop() {
  if (!running_) return;
  running_ = false;

  // Close all connections with shutdown code.
  auto conns = connections_;
  for (auto* conn : conns) {
    CloseWs(conn, 1001);
  }

  uv_timer_stop(&ping_timer_);
  uv_timer_stop(&stats_timer_);
  uv_timer_stop(&event_batch_timer_);
  uv_timer_stop(&log_batch_timer_);

  uv_close(reinterpret_cast<uv_handle_t*>(&ping_timer_), nullptr);
  uv_close(reinterpret_cast<uv_handle_t*>(&stats_timer_), nullptr);
  uv_close(reinterpret_cast<uv_handle_t*>(&event_batch_timer_), nullptr);
  uv_close(reinterpret_cast<uv_handle_t*>(&log_batch_timer_), nullptr);
  uv_close(reinterpret_cast<uv_handle_t*>(&async_event_), nullptr);
  uv_close(reinterpret_cast<uv_handle_t*>(&async_log_), nullptr);
}

int WsManager::active_connections() const {
  return connection_count_.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Connection acceptance
// ---------------------------------------------------------------------------

void WsManager::AcceptUpgrade(uv_stream_t* handle, std::string_view endpoint,
                              std::string_view client_key, int interval_ms) {
  if (!running_) {
    uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* h) {
      delete reinterpret_cast<uv_any_handle*>(h);
    });
    return;
  }

  if (active_connections() >= config_.max_connections) {
    // Reject: too many WS connections.  Send HTTP 503 and close.
    handler_->Warning("WS: max connections (%d) reached, rejecting upgrade",
                      config_.max_connections);
    uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* h) {
      delete reinterpret_cast<uv_any_handle*>(h);
    });
    return;
  }

  // Build and send the 101 handshake response.
  std::string handshake = WsBuildHandshakeResponse(client_key);

  auto* conn = new WsConnection;
  conn->manager = this;
  conn->endpoint = std::string(endpoint);
  // A tokenless server only pre-authenticates the stream when it was
  // explicitly opened; the HTTP layer refuses the handshake otherwise, and
  // this is the matching second line inside the WS layer.  read_open
  // pre-authenticates the read-only streams -- but not the log stream, which
  // carries the same lines as GET /v1/logs and keeps a configured token the
  // same way.  The handshake itself stays open for this path too (a browser
  // cannot carry Authorization on it); this is the only gate the log stream
  // gets.  Without a token (--api-no-auth, or the unix socket, where the
  // worker passes none) every stream is open.
  const bool tokenless_open =
      config_.auth_token.empty() && config_.allow_unauthenticated;
  const bool read_open_applies = config_.read_open && endpoint != "logs";
  conn->auth_required = !config_.auth_token.empty() && !read_open_applies;
  conn->authenticated = tokenless_open || read_open_applies;
  conn->stats_interval_ms =
      (interval_ms > 0) ? std::clamp(interval_ms, config_.stats_min_interval_ms,
                                     config_.stats_max_interval_ms)
                        : config_.stats_interval_ms;

  // Take ownership of the caller-allocated handle.
  conn->handle = handle;
  conn->handle->data = conn;

  connections_.push_back(conn);
  connection_count_.store(static_cast<int>(connections_.size()),
                          std::memory_order_relaxed);
  if (endpoint == "stats") {
    metrics_.stats_connections.fetch_add(1, std::memory_order_relaxed);
  } else if (endpoint == "logs") {
    metrics_.logs_connections.fetch_add(1, std::memory_order_relaxed);
  } else {
    metrics_.events_connections.fetch_add(1, std::memory_order_relaxed);
  }

  // Send handshake.
  auto* wctx = new WsWriteContext;
  wctx->conn = conn;
  wctx->data = std::move(handshake);
  wctx->buf = uv_buf_init(wctx->data.data(), wctx->data.size());

  conn->pending_writes++;
  int r = uv_write(&wctx->req, reinterpret_cast<uv_stream_t*>(conn->handle),
                   &wctx->buf, 1, OnWriteDone);
  if (r != 0) {
    conn->pending_writes--;
    delete wctx;
    CloseWs(conn, 0);
    return;
  }

  // Start auth timeout if auth is required.
  if (conn->auth_required) {
    uv_timer_init(loop_, &conn->auth_timer);
    conn->auth_timer.data = conn;
    uv_timer_start(&conn->auth_timer, OnAuthTimeout, config_.auth_timeout_ms,
                   0);
  }

  // Initialize pong timer (started when we send a ping).
  uv_timer_init(loop_, &conn->pong_timer);
  conn->pong_timer.data = conn;
  conn->timers_initialized = true;

  // Start reading.
  uv_read_start(reinterpret_cast<uv_stream_t*>(conn->handle), OnAlloc, OnRead);

  // If no auth required and this is a stats connection, send initial
  // snapshot immediately.
  if (conn->authenticated && conn->endpoint == "stats" && stats_provider_) {
    json current = stats_provider_();
    json snapshot;
    snapshot["type"] = "snapshot";
    snapshot["sequence"] = stats_seq_;
    snapshot["data"] = current;
    SendText(conn, snapshot.dump());
    // Only initialize the delta baseline if not yet set.
    if (prev_stats_.is_null()) {
      prev_stats_ = std::move(current);
    }
  }

  // If no auth required and this is a logs connection, send log snapshot.
  if (conn->authenticated && conn->endpoint == "logs") {
    SendLogSnapshot(conn);
  }
}

// ---------------------------------------------------------------------------
// Event posting (thread-safe)
// ---------------------------------------------------------------------------

void WsManager::PostEvent(std::string_view type, json detail) {
  if (!running_.load(std::memory_order_acquire)) return;

  json event;
  event["type"] = type;
  event["data"] = std::move(detail);

  {
    std::lock_guard<std::mutex> lock(event_mutex_);
    if (pending_events_.size() >= kMaxPendingQueueSize) return;
    pending_events_.push_back(std::move(event));
  }
  uv_async_send(&async_event_);
}

void WsManager::OnAsyncEvent(uv_async_t* async) {
  auto* self = static_cast<WsManager*>(async->data);
  self->DrainPendingEvents();
}

void WsManager::DrainPendingEvents() {
  // Events are batched by the event batch timer, but we drain the
  // pending queue into per-connection buffers here.
  std::vector<json> events;
  {
    std::lock_guard<std::mutex> lock(event_mutex_);
    events.swap(pending_events_);
  }

  for (auto* conn : connections_) {
    if (conn->closing || !conn->authenticated) continue;
    if (conn->endpoint != "events") continue;

    for (const auto& evt : events) {
      std::string serialized = evt.dump();
      if (conn->event_buffer_bytes + serialized.size() > config_.event_hwm) {
        // High-water mark exceeded, drop oldest events.
        metrics_.messages_dropped.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      if (!conn->event_buffer.empty()) {
        conn->event_buffer.push_back(',');
        conn->event_buffer_bytes += 1;
      }
      conn->event_buffer_bytes += serialized.size();
      conn->event_buffer +=
          std::move(serialized);  // NOLINT(performance-move-const-arg)
    }
  }
}

// ---------------------------------------------------------------------------
// Timer callbacks
// ---------------------------------------------------------------------------

void WsManager::OnPingTimer(uv_timer_t* timer) {
  auto* self = static_cast<WsManager*>(timer->data);
  for (auto* conn : self->connections_) {
    if (conn->closing || !conn->authenticated) continue;
    self->SendFrame(conn, WsBuildPingFrame());
    // Start pong timeout.
    uv_timer_start(&conn->pong_timer, OnPongTimeout,
                   self->config_.pong_timeout_ms, 0);
  }
}

void WsManager::OnStatsTimer(uv_timer_t* timer) {
  auto* self = static_cast<WsManager*>(timer->data);
  self->PushStats();
}

void WsManager::OnEventBatchTimer(uv_timer_t* timer) {
  auto* self = static_cast<WsManager*>(timer->data);
  self->FlushEventBatch();
}

void WsManager::OnAuthTimeout(uv_timer_t* timer) {
  auto* conn = static_cast<WsConnection*>(timer->data);
  if (!conn->authenticated && !conn->closing) {
    conn->manager->handler_->Warning("WS: auth timeout, closing");
    conn->manager->CloseWs(conn, 4001);
  }
}

void WsManager::OnPongTimeout(uv_timer_t* timer) {
  auto* conn = static_cast<WsConnection*>(timer->data);
  if (!conn->closing) {
    conn->manager->handler_->Warning("WS: pong timeout, closing");
    conn->manager->CloseWs(conn, 1001);
  }
}

// ---------------------------------------------------------------------------
// Stats push (delta computation)
// ---------------------------------------------------------------------------

void WsManager::PushStats() {
  if (!stats_provider_) return;

  bool has_stats_client = false;
  for (auto* conn : connections_) {
    if (!conn->closing && conn->authenticated && conn->endpoint == "stats") {
      has_stats_client = true;
      break;
    }
  }
  if (!has_stats_client) return;

  json current = stats_provider_();
  ++stats_seq_;

  // Compute delta: only include fields that changed.
  json delta;
  if (!prev_stats_.is_null()) {
    for (auto& [key, val] : current.items()) {
      if (!prev_stats_.contains(key) || prev_stats_[key] != val) {
        delta[key] = val;
      }
    }
  } else {
    delta = current;
  }

  if (delta.empty()) return;

  json msg;
  msg["type"] = "delta";
  msg["sequence"] = stats_seq_;
  msg["data"] = std::move(delta);
  std::string serialized = msg.dump();

  for (auto* conn : connections_) {
    if (conn->closing || !conn->authenticated) continue;
    if (conn->endpoint != "stats") continue;
    SendText(conn, serialized);
  }

  prev_stats_ = std::move(current);
}

// ---------------------------------------------------------------------------
// Event batch flush
// ---------------------------------------------------------------------------

void WsManager::FlushEventBatch() {
  // Increment sequence once per batch (not per-connection) so all clients
  // see the same sequence number for the same batch.
  bool has_data = false;
  for (const auto* conn : connections_) {
    if (!conn->closing && conn->authenticated && conn->endpoint == "events" &&
        !conn->event_buffer.empty()) {
      has_data = true;
      break;
    }
  }
  if (!has_data) return;
  ++events_seq_;

  for (auto* conn : connections_) {
    if (conn->closing || !conn->authenticated) continue;
    if (conn->endpoint != "events") continue;
    if (conn->event_buffer.empty()) continue;

    json msg;
    msg["type"] = "events";
    msg["sequence"] = events_seq_;
    // Send the raw newline-delimited events as a single text frame
    // wrapped in a JSON envelope.
    msg["data"] = json::parse("[" + conn->event_buffer + "]", nullptr, false);
    if (msg["data"].is_discarded()) {
      // Fallback: send as raw string.
      msg["data"] = conn->event_buffer;
    }

    SendText(conn, msg.dump());
    conn->event_buffer.clear();
    conn->event_buffer_bytes = 0;
  }
}

// ---------------------------------------------------------------------------
// libuv callbacks for WS connections
// ---------------------------------------------------------------------------

void WsManager::OnAlloc(uv_handle_t* /*handle*/, size_t suggested_size,
                        uv_buf_t* buf) {
  size_t size = std::min(suggested_size, size_t{65536});
  buf->base = new char[size];
  buf->len = size;
}

void WsManager::OnRead(uv_stream_t* stream, ssize_t nread,
                       const uv_buf_t* buf) {
  auto* conn = static_cast<WsConnection*>(stream->data);
  auto* self = conn->manager;

  if (nread < 0) {
    delete[] buf->base;
    self->CloseWs(conn, 0);
    return;
  }

  if (nread == 0) {
    delete[] buf->base;
    return;
  }

  std::string_view data(buf->base, static_cast<size_t>(nread));
  while (!data.empty() && !conn->closing) {
    size_t consumed = 0;
    auto result = conn->parser.Feed(data, consumed);
    data.remove_prefix(consumed);

    if (result == WsParseResult::kFrame) {
      self->HandleFrame(conn, conn->parser.frame());
      conn->parser.Reset();
    } else if (result == WsParseResult::kError) {
      delete[] buf->base;
      self->CloseWs(conn, 1002);
      return;
    }
  }
  delete[] buf->base;
}

void WsManager::OnWriteDone(uv_write_t* req, int status) {
  auto* wctx = reinterpret_cast<WsWriteContext*>(req);
  WsConnection* conn = wctx->conn;
  delete wctx;
  if (conn != nullptr) conn->pending_writes--;
  // Close connection on write error (broken pipe, reset, etc.).
  if (status < 0 && (conn != nullptr) && !conn->closing) {
    conn->manager->CloseWs(conn, 0);
  }
}

static void DeleteWsConnection(WsConnection* conn) {
  // Free the heap handle as the union it was allocated as.
  delete reinterpret_cast<uv_any_handle*>(conn->handle);
  delete conn;
}

void WsManager::OnClose(uv_handle_t* handle) {
  auto* conn = static_cast<WsConnection*>(handle->data);
  if (!conn) return;

  // Decrement the outstanding-close count.  The last close callback
  // to fire frees the connection.
  conn->pending_closes--;
  if (conn->pending_closes <= 0) {
    DeleteWsConnection(conn);
  }
}

// ---------------------------------------------------------------------------
// Frame handling
// ---------------------------------------------------------------------------

void WsManager::HandleFrame(WsConnection* conn, const WsFrame& frame) {
  switch (frame.opcode) {
    case WsOpcode::kText:
      if (!conn->authenticated) {
        HandleAuthMessage(conn, frame.payload);
      }
      // After auth, clients don't send meaningful text frames.
      break;

    case WsOpcode::kPong:
      // Cancel pong timeout.
      uv_timer_stop(&conn->pong_timer);
      break;

    case WsOpcode::kPing:
      // Reply with pong (echo payload).
      SendFrame(conn, WsBuildPongFrame(frame.payload));
      break;

    case WsOpcode::kClose:
      CloseWs(conn, (frame.close_code != 0u) ? frame.close_code : 1000);
      break;

    case WsOpcode::kBinary:
      CloseWs(conn, 1003);  // Unsupported data type.
      break;

    case WsOpcode::kContinuation:
      // No extensions negotiated; fragmented messages not supported.
      // Falls through to default for same handling.
    default:
      // Continuation frames and reserved opcodes (RFC 6455): protocol error.
      CloseWs(conn, 1002);
      break;
  }
}

void WsManager::HandleAuthMessage(WsConnection* conn,
                                  std::string_view payload) {
  // Expected: {"auth": "<token>"}
  // Guard against deeply nested JSON to prevent stack overflow in json::parse.
  {
    int depth = 0;
    for (char c : payload) {
      if (c == '{' || c == '[') {
        if (++depth > 32) {
          CloseWs(conn, 4001);
          return;
        }
      } else if (c == '}' || c == ']') {
        --depth;
      }
    }
  }
  auto msg = json::parse(payload, nullptr, false);
  if (msg.is_discarded() || !msg.contains("auth") || !msg["auth"].is_string()) {
    CloseWs(conn, 4001);
    return;
  }

  std::string token = msg["auth"].get<std::string>();
  if (!net_instaweb::ConstantTimeCompare(token, config_.auth_token)) {
    CloseWs(conn, 4001);
    return;
  }

  conn->authenticated = true;
  uv_timer_stop(&conn->auth_timer);

  // Send initial snapshot for stats connections.
  if (conn->endpoint == "stats" && stats_provider_) {
    json current = stats_provider_();
    json snapshot;
    snapshot["type"] = "snapshot";
    snapshot["sequence"] = stats_seq_;
    snapshot["data"] = current;
    SendText(conn, snapshot.dump());
    if (prev_stats_.is_null()) {
      prev_stats_ = std::move(current);
    }
  }

  // Send log snapshot for logs connections.
  if (conn->endpoint == "logs") {
    SendLogSnapshot(conn);
  }

  // Send auth success acknowledgment.
  json ack;
  ack["type"] = "auth_ok";
  SendText(conn, ack.dump());
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

void WsManager::SendText(WsConnection* conn, std::string_view text) {
  if (conn->closing) return;
  SendFrame(conn, WsBuildTextFrame(text));
  metrics_.messages_sent.fetch_add(1, std::memory_order_relaxed);
}

void WsManager::SendFrame(WsConnection* conn, const std::string& frame_data) {
  if (conn->closing) return;

  // Write backpressure: drop frame if too many writes are pending.
  // Prevents memory exhaustion from slow-reading clients.
  if (conn->pending_writes >= WsConnection::kMaxPendingWrites) {
    metrics_.messages_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  auto* wctx = new WsWriteContext;
  wctx->conn = conn;
  wctx->data = frame_data;
  wctx->buf = uv_buf_init(wctx->data.data(), wctx->data.size());
  conn->pending_writes++;

  int r = uv_write(&wctx->req, reinterpret_cast<uv_stream_t*>(conn->handle),
                   &wctx->buf, 1, OnWriteDone);
  if (r != 0) {
    conn->pending_writes--;
    delete wctx;
    CloseWs(conn, 0);
  }
}

// ---------------------------------------------------------------------------
// Connection close and cleanup
// ---------------------------------------------------------------------------

void WsManager::CloseWs(WsConnection* conn, uint16_t code) {
  if (conn->closing) return;
  conn->closing = true;

  // Best-effort close frame send before closing.
  if (code > 0) {
    std::string close_frame = WsBuildCloseFrame(code);
    auto* wctx = new WsWriteContext;
    wctx->conn = conn;
    wctx->data = std::move(close_frame);
    wctx->buf = uv_buf_init(wctx->data.data(), wctx->data.size());
    int r = uv_write(&wctx->req, reinterpret_cast<uv_stream_t*>(conn->handle),
                     &wctx->buf, 1, [](uv_write_t* req, int) {
                       delete reinterpret_cast<WsWriteContext*>(req);
                     });
    if (r != 0) delete wctx;
  }

  RemoveConnection(conn);
  uv_read_stop(reinterpret_cast<uv_stream_t*>(conn->handle));
  if (conn->timers_initialized) {
    uv_timer_stop(&conn->pong_timer);
    if (conn->auth_required) {
      uv_timer_stop(&conn->auth_timer);
    }
  }

  // Close all handles at once using a reference count.  Each OnClose
  // decrements pending_closes; the last one frees the connection.
  // This avoids the fragile nested-callback cascade that breaks when
  // uv_walk closes handles in an arbitrary order during force shutdown.
  conn->pending_closes = 0;
  if (SafeClose(conn->handle, OnClose)) conn->pending_closes++;
  if (conn->timers_initialized) {
    if (SafeClose(&conn->pong_timer, OnClose)) conn->pending_closes++;
    if (conn->auth_required && SafeClose(&conn->auth_timer, OnClose))
      conn->pending_closes++;
  }
  // If all handles were already closing (force shutdown raced us),
  // free the connection now.
  if (conn->pending_closes == 0) {
    DeleteWsConnection(conn);
  }
}

void WsManager::RemoveConnection(WsConnection* conn) {
  auto& v = connections_;
  v.erase(std::remove(v.begin(), v.end(), conn), v.end());
  connection_count_.store(static_cast<int>(v.size()),
                          std::memory_order_relaxed);

  if (conn->endpoint == "stats") {
    metrics_.stats_connections.fetch_sub(1, std::memory_order_relaxed);
  } else if (conn->endpoint == "logs") {
    metrics_.logs_connections.fetch_sub(1, std::memory_order_relaxed);
  } else {
    metrics_.events_connections.fetch_sub(1, std::memory_order_relaxed);
  }
}

// ---------------------------------------------------------------------------
// Log posting (thread-safe)
// ---------------------------------------------------------------------------

void WsManager::PostLog(std::string_view level, std::string_view source,
                        std::string_view module, std::string message) {
  if (!running_.load(std::memory_order_acquire)) return;

  auto now = std::chrono::system_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch())
                .count();

  json entry;
  entry["type"] = "log";
  entry["timestamp"] = ms;
  entry["source"] = source;
  entry["level"] = level;
  entry["module"] = module;
  entry["message"] = SanitizeLogMessage(std::move(message));

  {
    std::lock_guard<std::mutex> lock(log_mutex_);
    if (pending_logs_.size() >= kMaxPendingQueueSize) {
      // Shed without a seq (no phantom gap), but counted: /v1/logs reports
      // shed_total so a reader can say that entries were not kept.
      log_shed_total_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    pending_logs_.push_back(std::move(entry));
  }
  uv_async_send(&async_log_);
}

void WsManager::OnAsyncLog(uv_async_t* async) {
  auto* self = static_cast<WsManager*>(async->data);
  self->DrainPendingLogs();
}

void WsManager::DrainPendingLogs() {
  std::vector<json> logs;
  {
    std::lock_guard<std::mutex> lock(log_mutex_);
    logs.swap(pending_logs_);
  }

  for (auto& entry : logs) {
    // Stamp the per-process sequence number at drain time, on the loop
    // thread: ordering is the ring order by construction, no atomic is
    // needed, the retained seqs stay contiguous, and entries shed at a full
    // pending queue (PostLog's drop path) never receive one -- so a reader
    // never sees a phantom gap.
    entry["seq"] = log_total_;
    // Append to ring buffer, evicting oldest if at capacity.
    if (log_ring_.size() >= kMaxLogRingSize) {
      log_ring_.pop_front();
    }
    log_ring_.push_back(entry);
    ++log_total_;

    // Buffer into each logs connection.
    for (auto* conn : connections_) {
      if (conn->closing || !conn->authenticated) continue;
      if (conn->endpoint != "logs") continue;

      std::string serialized = DumpLog(entry);
      if (conn->log_buffer_bytes + serialized.size() > config_.event_hwm) {
        metrics_.messages_dropped.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      conn->log_buffer_bytes += serialized.size();
      conn->log_buffer.push_back(entry);
    }
  }
}

void WsManager::OnLogBatchTimer(uv_timer_t* timer) {
  auto* self = static_cast<WsManager*>(timer->data);
  self->FlushLogBatch();
}

void WsManager::FlushLogBatch() {
  for (auto* conn : connections_) {
    if (conn->closing || !conn->authenticated) continue;
    if (conn->endpoint != "logs") continue;
    if (conn->log_buffer.empty()) continue;

    // Send individual log entries (not batched array — matches
    // frontend protocol expectation).
    for (auto& entry : conn->log_buffer) {
      SendText(conn, DumpLog(entry));
    }
    conn->log_buffer.clear();
    conn->log_buffer_bytes = 0;
  }
}

void WsManager::SendLogSnapshot(WsConnection* conn) {
  json snapshot;
  snapshot["type"] = "snapshot";
  snapshot["entries"] = json::array();
  for (const auto& entry : log_ring_) {
    snapshot["entries"].push_back(entry);
  }
  snapshot["total"] = log_total_;
  SendText(conn, DumpLog(snapshot));
}

nlohmann::json WsManager::BuildLogsResponse(uint64_t since, bool has_since,
                                            size_t limit) const {
  // Loop thread only, like SendLogSnapshot: log_ring_ is confined to the
  // event loop, and route handlers run on that loop, so this reads without a
  // lock.  Never blocks; the work is proportional to the page, never to the
  // ring.
  const size_t size = log_ring_.size();
  const uint64_t oldest_seq =
      size == 0 ? 0 : log_ring_.front().at("seq").get<uint64_t>();
  const uint64_t newest_seq =
      size == 0 ? 0 : log_ring_.back().at("seq").get<uint64_t>();

  // One entry's serialized size inside the page, its separating comma
  // included.  The budget counts real bytes, JSON escaping and all, so the
  // bound is exact.
  const auto entry_bytes = [](const json& entry) {
    return DumpLog(entry).size() + 1;
  };
  size_t used = kLogsPageEnvelopeReserve;

  // The page is log_ring_[first, last).
  size_t first = size;
  size_t last = size;
  if (has_since) {
    // Retained seqs are contiguous (one per drained entry), so the first
    // entry newer than the cursor sits at index since + 1 - oldest_seq; a
    // cursor older than the ring starts at its oldest entry, and one at or
    // past the newest matches nothing.
    if (size > 0 && since < newest_seq) {
      first =
          since < oldest_seq ? 0 : static_cast<size_t>(since + 1 - oldest_seq);
    }
    last = first;
    // Oldest unseen first, so a burst larger than one page is paged through
    // on later reads (`more`) rather than dropped from the middle.
    while (last < size && last - first < limit) {
      const size_t n = entry_bytes(log_ring_[last]);
      if (last > first && used + n > kMaxLogsPageBytes) break;
      used += n;
      ++last;
    }
  } else {
    // No cursor: the newest entries that fit, walking back from the newest,
    // so "what is happening now" is never what the budget leaves out.
    while (first > 0 && last - first < limit) {
      const size_t n = entry_bytes(log_ring_[first - 1]);
      if (first < last && used + n > kMaxLogsPageBytes) break;
      used += n;
      --first;
    }
  }

  json entries = json::array();
  for (size_t i = first; i < last; ++i) {
    entries.push_back(log_ring_[i]);
  }

  json resp;
  resp["entries"] = std::move(entries);
  resp["next_since"] = last > first
                           ? log_ring_[last - 1].at("seq").get<uint64_t>()
                           : (has_since ? since : uint64_t{0});
  resp["oldest_seq"] = oldest_seq;
  resp["newest_seq"] = newest_seq;
  // The cursor fell behind the oldest retained entry: the entries in
  // between were evicted.  (oldest_seq >= 1 keeps the subtraction safe and
  // an empty ring a non-gap.)
  resp["gap"] = has_since && oldest_seq >= 1 && since < oldest_seq - 1;
  resp["more"] = has_since && last < size;
  resp["shed_total"] = log_shed_total_.load(std::memory_order_relaxed);
  resp["stream_id"] = stream_id_;
  return resp;
}

}  // namespace pagespeed
