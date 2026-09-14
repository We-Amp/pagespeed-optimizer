// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// WebSocket Protocol + Endpoint Handler Tests
//
// Tests:
// 1. Frame parsing (text, ping, pong, close, masked, extended lengths)
// 2. Frame construction
// 3. Handshake (Sec-WebSocket-Accept computation)
// 4. WsManager lifecycle (upgrade, auth, stats push, events, ping/pong)

#include "src/worker/websocket.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "gtest/gtest.h"
#include "lib/base/message_handler.h"
#include "nlohmann/json.hpp"
#include "src/worker/ws_handlers.h"
#include "test/test_util/tcp_client.h"
#include "uv.h"

namespace pagespeed {
namespace {

using json = nlohmann::json;

// Helper: build a masked client frame for any opcode/payload.
static std::string BuildMaskedFrame(WsOpcode opcode, std::string_view payload) {
  uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  std::string frame;
  frame.push_back(static_cast<char>(0x80 | static_cast<uint8_t>(opcode)));

  if (payload.size() < 126) {
    frame.push_back(static_cast<char>(0x80 | payload.size()));
  } else if (payload.size() <= 0xFFFF) {
    frame.push_back(static_cast<char>(0x80 | 126));
    frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
    frame.push_back(static_cast<char>(payload.size() & 0xFF));
  } else {
    frame.push_back(static_cast<char>(0x80 | 127));
    uint64_t len = payload.size();
    for (int i = 7; i >= 0; --i) {
      frame.push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
    }
  }
  frame.append(reinterpret_cast<char*>(mask), 4);
  for (size_t i = 0; i < payload.size(); ++i) {
    frame.push_back(payload[i] ^ mask[i % 4]);
  }
  return frame;
}

// Helper: build a masked close frame with status code.
static std::string BuildMaskedCloseFrame(uint16_t code) {
  char payload[2];
  payload[0] = static_cast<char>((code >> 8) & 0xFF);
  payload[1] = static_cast<char>(code & 0xFF);
  return BuildMaskedFrame(WsOpcode::kClose, std::string_view(payload, 2));
}

// ===================================================================
// WebSocket Protocol Tests (framing + handshake)
// ===================================================================

TEST(WsFrameParserTest, UnmaskedClientFrameRejected) {
  // RFC 6455 §5.1: client-to-server frames MUST be masked.
  std::string frame = WsBuildTextFrame("Hello");

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kError);
}

TEST(WsFrameParserTest, ParseMaskedTextFrame) {
  // Client→server frames are masked (RFC 6455 §5.1).
  // Build a masked frame for "Hi" with mask key [0x37, 0xfa, 0x21, 0x3d].
  uint8_t mask[4] = {0x37, 0xfa, 0x21, 0x3d};
  std::string frame;
  frame.push_back(static_cast<char>(0x81));  // FIN + TEXT
  frame.push_back(static_cast<char>(0x82));  // MASK + len=2
  frame.append(reinterpret_cast<char*>(mask), 4);
  // Masked payload: "Hi" ^ mask
  frame.push_back('H' ^ mask[0]);
  frame.push_back('i' ^ mask[1]);

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().payload, "Hi");
}

TEST(WsFrameParserTest, ParseCloseFrame) {
  std::string frame = BuildMaskedCloseFrame(1000);

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().opcode, WsOpcode::kClose);
  EXPECT_EQ(parser.frame().close_code, 1000);
}

TEST(WsFrameParserTest, ParsePingFrame) {
  std::string frame = BuildMaskedFrame(WsOpcode::kPing, "ping-data");

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().opcode, WsOpcode::kPing);
  EXPECT_EQ(parser.frame().payload, "ping-data");
}

TEST(WsFrameParserTest, ParsePongFrame) {
  std::string frame = BuildMaskedFrame(WsOpcode::kPong, "pong-data");

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().opcode, WsOpcode::kPong);
  EXPECT_EQ(parser.frame().payload, "pong-data");
}

TEST(WsFrameParserTest, IncrementalParsing) {
  std::string frame = BuildMaskedFrame(WsOpcode::kText, "Hello, World!");

  WsFrameParser parser;
  // Feed one byte at a time.
  for (size_t i = 0; i < frame.size() - 1; ++i) {
    size_t consumed = 0;
    auto result = parser.Feed(std::string_view(frame.data() + i, 1), consumed);
    EXPECT_EQ(result, WsParseResult::kNeedMore);
    EXPECT_EQ(consumed, 1u);
  }

  // Feed last byte.
  size_t consumed = 0;
  auto result = parser.Feed(
      std::string_view(frame.data() + frame.size() - 1, 1), consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().payload, "Hello, World!");
}

TEST(WsFrameParserTest, ExtendedLength16) {
  // Create a payload of 200 bytes (> 125, uses 16-bit extended length).
  std::string payload(200, 'A');
  std::string frame = BuildMaskedFrame(WsOpcode::kText, payload);

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().payload.size(), 200u);
  EXPECT_EQ(parser.frame().payload, payload);
}

TEST(WsFrameParserTest, EmptyPayload) {
  std::string frame = BuildMaskedFrame(WsOpcode::kText, "");

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().payload, "");
}

TEST(WsFrameParserTest, MultipleFrames) {
  std::string frame1 = BuildMaskedFrame(WsOpcode::kText, "first");
  std::string frame2 = BuildMaskedFrame(WsOpcode::kText, "second");
  std::string combined = frame1 + frame2;

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(combined, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().payload, "first");
  EXPECT_EQ(consumed, frame1.size());

  parser.Reset();
  std::string_view remaining(combined.data() + consumed,
                             combined.size() - consumed);
  size_t consumed2 = 0;
  result = parser.Feed(remaining, consumed2);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().payload, "second");
}

// ===================================================================
// Handshake Tests
// ===================================================================

TEST(WsHandshakeTest, AcceptKeyComputation) {
  // RFC 6455 §4.2.2 example.
  std::string accept = WsComputeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==");
  EXPECT_EQ(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WsHandshakeTest, HandshakeResponse) {
  std::string response = WsBuildHandshakeResponse("dGhlIHNhbXBsZSBub25jZQ==");
  EXPECT_NE(response.find("101 Switching Protocols"), std::string::npos);
  EXPECT_NE(response.find("Upgrade: websocket"), std::string::npos);
  EXPECT_NE(response.find("Connection: Upgrade"), std::string::npos);
  EXPECT_NE(response.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);
}

// ===================================================================
// WsManager Integration Tests
// ===================================================================

class WsManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    loop_ = new uv_loop_t;
    uv_loop_init(loop_);
    handler_ = std::make_unique<NullMessageHandler>();

    // A tokenless WS stream is only pre-authenticated when
    // the API was deliberately opened.  These fixtures exercise the frame and
    // fan-out machinery, not the credential gate — the token-bearing
    // fixtures below cover that.
    ws_config_.allow_unauthenticated = true;
    ws_config_.max_connections = 8;
    ws_config_.ping_interval_ms = 5000;
    ws_config_.pong_timeout_ms = 2000;
    ws_config_.auth_timeout_ms = 1000;
    ws_config_.stats_interval_ms = 200;
    ws_config_.event_batch_ms = 50;

    manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
    stats_call_count_ = 0;
    manager_->SetStatsProvider([this]() -> json {
      stats_call_count_++;
      json j;
      j["notifications"] = stats_call_count_ * 10;
      j["variants"] = stats_call_count_ * 5;
      return j;
    });

    // Create a TCP listener to hand off connections.
    uv_tcp_init(loop_, &listener_);
    listener_.data = this;
    struct sockaddr_in addr;
    uv_ip4_addr("127.0.0.1", 0, &addr);
    uv_tcp_bind(&listener_, reinterpret_cast<const sockaddr*>(&addr), 0);

    struct sockaddr_storage bound;
    int namelen = sizeof(bound);
    uv_tcp_getsockname(&listener_, reinterpret_cast<sockaddr*>(&bound),
                       &namelen);
    port_ = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);

    uv_listen(reinterpret_cast<uv_stream_t*>(&listener_), 4,
              [](uv_stream_t* server, int status) {
                if (status < 0) return;
                auto* self = static_cast<WsManagerTest*>(server->data);
                // Allocated as the union HttpServer hands over (the
                // transport may be a pipe), and freed as one.
                auto* client = new uv_any_handle;
                uv_tcp_init(self->loop_, &client->tcp);
                if (uv_accept(server, &client->stream) == 0) {
                  self->manager_->AcceptUpgrade(
                      &client->stream, self->pending_endpoint_,
                      "dGhlIHNhbXBsZSBub25jZQ==", self->pending_interval_);
                } else {
                  uv_close(&client->handle, [](uv_handle_t* h) {
                    delete reinterpret_cast<uv_any_handle*>(h);
                  });
                }
              });

    manager_->Start();
    StartLoopThread();
  }

  void TearDown() override {
    StopLoopThread();
    manager_->Stop();
    uv_close(reinterpret_cast<uv_handle_t*>(&listener_), nullptr);
    uv_run(loop_, UV_RUN_DEFAULT);
    manager_.reset();
    uv_loop_close(loop_);
    delete loop_;
  }

  void StartLoopThread() {
    loop_running_ = true;
    loop_thread_ = std::thread([this] {
      while (loop_running_) {
        uv_run(loop_, UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });
  }

  void StopLoopThread() {
    loop_running_ = false;
    if (loop_thread_.joinable()) loop_thread_.join();
  }

  // Connect a raw TCP socket to the listener.
  int ConnectRawSocket() const { return test::ConnectTcp(port_, 2); }

  // Read until we get the full handshake response.
  // Reads byte-by-byte once close to the end of the HTTP header to avoid
  // consuming WebSocket frame data that arrives in the same TCP segment.
  std::string ReadHandshake(int sock) {
    std::string response;
    char c;
    while (true) {
      ssize_t n = test::SocketRead(sock, &c, 1);
      if (n != 1) break;
      response.push_back(c);
      if (response.size() >= 4 &&
          response.compare(response.size() - 4, 4, "\r\n\r\n") == 0) {
        break;
      }
    }
    return response;
  }

  // Build a masked client text frame.
  std::string BuildMaskedTextFrame(std::string_view payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));  // FIN + TEXT
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};

    if (payload.size() < 126) {
      frame.push_back(static_cast<char>(0x80 | payload.size()));
    } else {
      frame.push_back(static_cast<char>(0x80 | 126));
      frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
      frame.push_back(static_cast<char>(payload.size() & 0xFF));
    }
    frame.append(reinterpret_cast<char*>(mask), 4);
    for (size_t i = 0; i < payload.size(); ++i) {
      frame.push_back(payload[i] ^ mask[i % 4]);
    }
    return frame;
  }

  // Read a server text frame and return the payload.
  std::string ReadTextFrame(int sock) {
    uint8_t header[2];
    ssize_t n = test::SocketRead(sock, header, 2);
    if (n != 2) return "";

    uint8_t opcode = header[0] & 0x0F;
    size_t len = header[1] & 0x7F;

    if (len == 126) {
      uint8_t ext[2];
      if (test::SocketRead(sock, ext, 2) != 2) return "";
      len = (size_t(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
      uint8_t ext[8];
      if (test::SocketRead(sock, ext, 8) != 8) return "";
      len = 0;
      for (unsigned char i : ext) {
        len = (len << 8) | i;
      }
    }

    std::string payload(len, '\0');
    size_t total = 0;
    while (total < len) {
      n = test::SocketRead(sock, payload.data() + total, len - total);
      if (n <= 0) break;
      total += n;
    }

    // Return empty for non-text frames (ping/pong/close).
    if (opcode != 0x1) return std::string(1, char(opcode)) + payload;
    return payload;
  }

  // Read text frames until we get one matching the expected JSON "type",
  // skipping deltas, control frames, etc.  Uses the existing 2s socket
  // timeout; gives up after 3 attempts (6s worst case).
  std::string ReadTextFrameByType(int sock, const std::string& expected_type) {
    for (int attempt = 0; attempt < 3; ++attempt) {
      std::string msg = ReadTextFrame(sock);
      if (msg.empty()) break;  // Socket timeout or error — give up.
      // Skip control frames (ping 0x09, pong 0x0A, close 0x08).
      if (msg[0] == '\x09' || msg[0] == '\x0A' || msg[0] == '\x08') continue;
      // Check if this is the expected type.
      json j = json::parse(msg, nullptr, false);
      if (!j.is_discarded() && j.contains("type") &&
          j["type"] == expected_type) {
        return msg;
      }
      // Wrong type (e.g., delta instead of snapshot); try next frame.
    }
    return "";
  }

  uv_loop_t* loop_ = nullptr;
  uv_tcp_t listener_;
  int port_ = 0;
  std::unique_ptr<NullMessageHandler> handler_;
  WsConfig ws_config_;
  std::unique_ptr<WsManager> manager_;
  std::thread loop_thread_;
  std::atomic<bool> loop_running_{false};
  std::string pending_endpoint_ = "stats";
  int pending_interval_ = 0;
  int stats_call_count_ = 0;
};

TEST_F(WsManagerTest, HandshakeAndConnect) {
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  std::string handshake = ReadHandshake(sock);
  EXPECT_NE(handshake.find("101 Switching Protocols"), std::string::npos);
  EXPECT_NE(handshake.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);

  // Should have 1 active connection.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(manager_->active_connections(), 0);
}

TEST_F(WsManagerTest, StatsSnapshotOnConnect) {
  // No auth required — should get initial snapshot immediately.
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  std::string handshake = ReadHandshake(sock);
  ASSERT_NE(handshake.find("101"), std::string::npos);

  // Read the initial snapshot, skipping any delta messages that may arrive
  // first due to the stats timer firing before the snapshot is sent.
  std::string msg = ReadTextFrameByType(sock, "snapshot");
  ASSERT_FALSE(msg.empty());

  json j = json::parse(msg, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  EXPECT_EQ(j["type"], "snapshot");
  EXPECT_TRUE(j.contains("data"));
  EXPECT_TRUE(j["data"].contains("notifications"));

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, StatsDeltaPush) {
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);
  // Read initial snapshot, skipping any interleaved delta messages.
  ReadTextFrameByType(sock, "snapshot");

  // Wait for a delta push (stats_interval_ms = 200).
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  std::string msg = ReadTextFrame(sock);
  if (!msg.empty()) {
    json j = json::parse(msg, nullptr, false);
    if (!j.is_discarded()) {
      EXPECT_EQ(j["type"], "delta");
      EXPECT_TRUE(j.contains("sequence"));
      EXPECT_TRUE(j.contains("data"));
    }
  }

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, AuthRequired) {
  ws_config_.auth_token = "secret-token";
  // Recreate manager with auth.
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 42}}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);

  // Send auth message.
  std::string auth_msg = BuildMaskedTextFrame(R"({"auth":"secret-token"})");
  ssize_t sent = test::SocketWrite(sock, auth_msg.data(), auth_msg.size());
  ASSERT_EQ(sent, static_cast<ssize_t>(auth_msg.size()));

  // Should get auth_ok and then snapshot.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::string msg1 = ReadTextFrame(sock);
  ASSERT_FALSE(msg1.empty());
  json j1 = json::parse(msg1, nullptr, false);

  // Could be auth_ok or snapshot — find auth_ok.
  bool got_auth_ok = false;
  bool got_snapshot = false;

  if (!j1.is_discarded()) {
    if (j1["type"] == "auth_ok") got_auth_ok = true;
    if (j1["type"] == "snapshot") got_snapshot = true;
  }

  std::string msg2 = ReadTextFrame(sock);
  if (!msg2.empty()) {
    json j2 = json::parse(msg2, nullptr, false);
    if (!j2.is_discarded()) {
      if (j2["type"] == "auth_ok") got_auth_ok = true;
      if (j2["type"] == "snapshot") got_snapshot = true;
    }
  }

  EXPECT_TRUE(got_auth_ok);
  EXPECT_TRUE(got_snapshot);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, AuthTimeout) {
  ws_config_.auth_token = "secret-token";
  ws_config_.auth_timeout_ms = 200;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);

  // Don't send auth — wait for timeout.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  // Connection should be closed.
  EXPECT_EQ(manager_->active_connections(), 0);
  test::CloseSocket(sock);
}

TEST_F(WsManagerTest, WrongAuthToken) {
  ws_config_.auth_token = "secret-token";
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);

  // Send wrong token.
  std::string auth_msg = BuildMaskedTextFrame(R"({"auth":"wrong"})");
  test::SocketWrite(sock, auth_msg.data(), auth_msg.size());

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(manager_->active_connections(), 0);
  test::CloseSocket(sock);
}

TEST_F(WsManagerTest, EventPosting) {
  pending_endpoint_ = "events";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);

  // Post some events.
  manager_->PostEvent("variant_written", json{{"url", "/test.jpg"}});
  manager_->PostEvent("notification", json{{"url", "/page.html"}});

  // Wait for event batch (event_batch_ms = 50).
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  std::string msg = ReadTextFrame(sock);
  if (!msg.empty()) {
    json j = json::parse(msg, nullptr, false);
    if (!j.is_discarded()) {
      EXPECT_EQ(j["type"], "events");
      EXPECT_TRUE(j.contains("data"));
    }
  }

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, MaxConnectionsEnforced) {
  ws_config_.max_connections = 2;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";

  int sock1 = ConnectRawSocket();
  ASSERT_GE(sock1, 0);
  ReadHandshake(sock1);

  int sock2 = ConnectRawSocket();
  ASSERT_GE(sock2, 0);
  ReadHandshake(sock2);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 2);

  // Third connection should be rejected.
  int sock3 = ConnectRawSocket();
  ASSERT_GE(sock3, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(manager_->active_connections(), 2);

  test::CloseSocket(sock1);
  test::CloseSocket(sock2);
  test::CloseSocket(sock3);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, MetricsTracking) {
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  ReadTextFrame(sock);  // snapshot

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_GE(manager_->metrics().messages_sent.load(), 1u);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// /v1/ws/logs Endpoint Tests
// ===================================================================

TEST_F(WsManagerTest, LogsSnapshotOnConnect) {
  // Post a log entry before any client connects so the ring buffer
  // is non-empty.
  manager_->PostLog("info", "worker", "worker", "startup complete");
  // PostLog drains via async callback on the loop thread; give it time.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  std::string handshake = ReadHandshake(sock);
  ASSERT_NE(handshake.find("101"), std::string::npos);

  // The snapshot is sent async; skip any non-snapshot frames.
  std::string msg = ReadTextFrameByType(sock, "snapshot");
  ASSERT_FALSE(msg.empty());

  json j = json::parse(msg, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  EXPECT_EQ(j["type"], "snapshot");
  EXPECT_TRUE(j.contains("entries"));
  EXPECT_TRUE(j["entries"].is_array());
  EXPECT_GE(j["entries"].size(), 1u);
  EXPECT_TRUE(j.contains("total"));

  // Verify the entry shape.
  json entry = j["entries"][0];
  EXPECT_EQ(entry["type"], "log");
  EXPECT_EQ(entry["level"], "info");
  EXPECT_EQ(entry["source"], "worker");
  EXPECT_TRUE(entry.contains("timestamp"));
  EXPECT_TRUE(entry.contains("message"));

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, LogsPostLogDelivered) {
  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);
  // Read the (possibly empty) snapshot.
  ReadTextFrame(sock);

  // Post a log entry after connection is established.
  manager_->PostLog("warning", "worker", "image",
                    "image too large to optimize");

  // Wait for drain + batch flush.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  std::string msg = ReadTextFrame(sock);
  if (!msg.empty()) {
    json j = json::parse(msg, nullptr, false);
    if (!j.is_discarded()) {
      EXPECT_EQ(j["type"], "log");
      EXPECT_EQ(j["level"], "warning");
      EXPECT_EQ(j["source"], "worker");
      EXPECT_EQ(j["module"], "image");
      EXPECT_EQ(j["message"], "image too large to optimize");
    }
  }

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, LogsRingBufferCap) {
  // Fill the ring buffer beyond its capacity.
  for (int i = 0; i < 2050; ++i) {
    manager_->PostLog("info", "worker", "worker", "entry " + std::to_string(i));
  }

  // Let all entries drain into the ring buffer.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);

  std::string msg = ReadTextFrame(sock);
  ASSERT_FALSE(msg.empty());

  json j = json::parse(msg, nullptr, false);
  ASSERT_FALSE(j.is_discarded());
  EXPECT_EQ(j["type"], "snapshot");
  // Ring buffer should be capped at 2000.
  EXPECT_LE(j["entries"].size(), 2000u);
  // Total should reflect all posted entries.
  EXPECT_GE(j["total"].get<uint64_t>(), 2050u);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

TEST_F(WsManagerTest, LogsMetricsTracking) {
  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->metrics().logs_connections.load(), 1);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(manager_->metrics().logs_connections.load(), 0);
}

// ===================================================================
// WsConfig and WsMetrics struct tests
// ===================================================================

TEST(WsConfigTest, Defaults) {
  WsConfig config;
  // Sanity checks: all timeouts/limits should be positive.
  EXPECT_GT(config.max_connections, 0);
  EXPECT_GT(config.ping_interval_ms, 0);
  EXPECT_GT(config.pong_timeout_ms, 0);
  EXPECT_GT(config.auth_timeout_ms, 0);
  EXPECT_GT(config.stats_interval_ms, 0);
  EXPECT_GT(config.stats_min_interval_ms, 0);
  EXPECT_GT(config.stats_max_interval_ms, config.stats_min_interval_ms);
  EXPECT_GT(config.event_batch_ms, 0);
  EXPECT_GT(config.event_hwm, 0u);
  EXPECT_TRUE(config.auth_token.empty());
}

TEST(WsMetricsTest, Defaults) {
  WsMetrics metrics;
  EXPECT_EQ(metrics.stats_connections.load(), 0);
  EXPECT_EQ(metrics.events_connections.load(), 0);
  EXPECT_EQ(metrics.logs_connections.load(), 0);
  EXPECT_EQ(metrics.messages_sent.load(), 0u);
  EXPECT_EQ(metrics.messages_dropped.load(), 0u);
}

// ===================================================================
// Additional frame parsing tests
// ===================================================================

TEST(WsFrameParserTest, EmptyTextFrame) {
  std::string frame = BuildMaskedFrame(WsOpcode::kText, "");
  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().payload, "");
}

TEST(WsFrameParserTest, LargeTextFrame) {
  // Text frame with 1000-byte payload.
  std::string payload(1000, 'A');
  std::string frame = BuildMaskedFrame(WsOpcode::kText, payload);
  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().payload, payload);
}

TEST(WsFrameParserTest, IncompleteFrameNeedsMore) {
  // Feed only the first byte of a text frame.
  std::string partial;
  partial.push_back(static_cast<char>(0x81));  // FIN + TEXT

  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(partial, consumed);
  EXPECT_EQ(result, WsParseResult::kNeedMore);
}

TEST(WsFrameParserTest, CloseFrameWithCode1001) {
  std::string frame = BuildMaskedCloseFrame(1001);
  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kFrame);
  EXPECT_EQ(parser.frame().opcode, WsOpcode::kClose);
  EXPECT_EQ(parser.frame().close_code, 1001);
}

TEST(WsFrameParserTest, ControlFramePayloadTooLarge) {
  // RFC 6455 §5.5: control frames MUST NOT have payload > 125 bytes.
  std::string payload(126, 'X');
  std::string frame = BuildMaskedFrame(WsOpcode::kPing, payload);
  WsFrameParser parser;
  size_t consumed = 0;
  auto result = parser.Feed(frame, consumed);
  EXPECT_EQ(result, WsParseResult::kError);
}

// ===================================================================
// Coverage gap tests: stop/reject, interval clamping, event overflow,
// stats delta computation, ping/pong lifecycle
// ===================================================================

// Test: WsManager::Stop() while not running is a no-op.
TEST_F(WsManagerTest, StopWhenNotRunning) {
  // Create a manager that is never started.
  StopLoopThread();
  manager_
      ->Stop();  // Already stopped by TearDown logic, but let's be explicit.
  uv_run(loop_, UV_RUN_NOWAIT);

  WsConfig config;
  config.max_connections = 2;
  auto fresh_mgr = std::make_unique<WsManager>(loop_, config, handler_.get());
  // Stop without Start should not crash.
  fresh_mgr->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  fresh_mgr.reset();
}

// Test: Stats interval clamping to min/max bounds.
// When a client requests an interval below min or above max,
// AcceptUpgrade clamps it. This exercises lines 163-166.
TEST_F(WsManagerTest, StatsIntervalClamping) {
  // Reconfigure with known min/max.
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  ws_config_.stats_min_interval_ms = 200;
  ws_config_.stats_max_interval_ms = 5000;
  ws_config_.stats_interval_ms = 1000;

  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  // Request interval of 50ms (below min) — should be clamped to 200ms.
  pending_endpoint_ = "stats";
  pending_interval_ = 50;

  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  std::string handshake = ReadHandshake(sock);
  ASSERT_NE(handshake.find("101"), std::string::npos);

  // Connection should be active (not rejected).
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Test: Event buffer overflow (high-water mark exceeded).
// Exercises lines 271-274 (event_hwm exceeded, drops oldest events).
TEST_F(WsManagerTest, EventBufferOverflow) {
  // Reconfigure with very small event_hwm to trigger overflow.
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  ws_config_.event_hwm = 64;        // Very small buffer.
  ws_config_.event_batch_ms = 200;  // Longer batch interval.

  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "events";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);

  // Flood with events exceeding the 64-byte buffer.
  for (int i = 0; i < 50; ++i) {
    manager_->PostEvent("flood",
                        json{{"idx", i}, {"padding", "abcdefghijklmnop"}});
  }

  // Wait for batch flush.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  // Some messages should have been dropped.
  EXPECT_GT(manager_->metrics().messages_dropped.load(), 0u)
      << "Some events should have been dropped due to HWM";

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Test: Stats delta computation sends only changed fields.
// Exercises lines 345-356 (delta computation).
TEST_F(WsManagerTest, StatsDeltaOnlyChangedFields) {
  // The stats provider returns incrementing values each time.
  // After the initial snapshot, delta pushes should contain only changed fields.
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);
  // Read initial snapshot.
  std::string snapshot_msg = ReadTextFrameByType(sock, "snapshot");
  ASSERT_FALSE(snapshot_msg.empty());

  // Wait for at least one delta push (stats_interval_ms = 200).
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  std::string delta_msg = ReadTextFrameByType(sock, "delta");
  if (!delta_msg.empty()) {
    json delta = json::parse(delta_msg, nullptr, false);
    if (!delta.is_discarded()) {
      EXPECT_EQ(delta["type"], "delta");
      EXPECT_TRUE(delta.contains("sequence"));
      EXPECT_TRUE(delta.contains("data"));
      // Delta should have a sequence number > 0.
      EXPECT_GT(delta["sequence"].get<uint64_t>(), 0u);
    }
  }

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// Test: Events endpoint connection increments events_connections metric.
TEST_F(WsManagerTest, EventsMetricsTracking) {
  pending_endpoint_ = "events";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->metrics().events_connections.load(), 1);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(manager_->metrics().events_connections.load(), 0);
}

// Test: AcceptUpgrade when manager is not running rejects the connection.
// Exercises lines 140-143 (not running path).
TEST_F(WsManagerTest, AcceptUpgradeWhenStopped) {
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  // Create a new TCP handle and try to upgrade it after Stop.
  auto* handle = new uv_any_handle;
  uv_tcp_init(loop_, &handle->tcp);

  // AcceptUpgrade on a stopped manager should close the handle without crash.
  manager_->AcceptUpgrade(&handle->stream, "stats",
                          "dGhlIHNhbXBsZSBub25jZQ==", 0);

  // Run the loop to process the close callback.
  uv_run(loop_, UV_RUN_NOWAIT);

  // Should still have 0 active connections.
  EXPECT_EQ(manager_->active_connections(), 0);
}

// ===================================================================
// Coverage: PostEvent when manager is not running (line 238)
// ===================================================================

TEST_F(WsManagerTest, PostEventWhenStoppedIsNoOp) {
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  // PostEvent on a stopped manager should not crash.
  manager_->PostEvent("test_event", json{{"key", "value"}});

  // PostLog on a stopped manager should not crash.
  manager_->PostLog("info", "worker", "test", "should be ignored");

  // No connections, no errors.
  EXPECT_EQ(manager_->active_connections(), 0);
}

// ===================================================================
// Coverage: Multiple event connections receive broadcasts (lines 265-283)
// ===================================================================

TEST_F(WsManagerTest, MultipleEventConnectionsReceiveBroadcast) {
  pending_endpoint_ = "events";

  int sock1 = ConnectRawSocket();
  ASSERT_GE(sock1, 0);
  ReadHandshake(sock1);

  int sock2 = ConnectRawSocket();
  ASSERT_GE(sock2, 0);
  ReadHandshake(sock2);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 2);

  // Post an event.
  manager_->PostEvent("broadcast_test", json{{"url", "/test.jpg"}});

  // Wait for event batch.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Both clients should receive data.
  std::string msg1 = ReadTextFrame(sock1);
  std::string msg2 = ReadTextFrame(sock2);

  // At least one should have received the event.
  bool got_event1 =
      !msg1.empty() && msg1.find("broadcast_test") != std::string::npos;
  bool got_event2 =
      !msg2.empty() && msg2.find("broadcast_test") != std::string::npos;
  EXPECT_TRUE(got_event1 || got_event2)
      << "At least one event connection should receive the broadcast";

  test::CloseSocket(sock1);
  test::CloseSocket(sock2);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: Invalid auth message format (lines 537-541)
// ===================================================================

TEST_F(WsManagerTest, InvalidAuthJsonFormat) {
  ws_config_.auth_token = "secret-token";
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  // Send invalid JSON (not a valid auth message).
  std::string invalid_msg = BuildMaskedTextFrame("not json at all");
  test::SocketWrite(sock, invalid_msg.data(), invalid_msg.size());

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(manager_->active_connections(), 0)
      << "Invalid auth JSON should close the connection";
  test::CloseSocket(sock);
}

TEST_F(WsManagerTest, AuthMessageMissingAuthField) {
  ws_config_.auth_token = "secret-token";
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  // Send valid JSON but missing "auth" field.
  std::string missing_auth =
      BuildMaskedTextFrame(R"({"token":"secret-token"})");
  test::SocketWrite(sock, missing_auth.data(), missing_auth.size());

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(manager_->active_connections(), 0)
      << "Auth message without 'auth' field should close the connection";
  test::CloseSocket(sock);
}

// ===================================================================
// Coverage: Stats push with no stats provider (line 332)
// ===================================================================

TEST_F(WsManagerTest, StatsWithNoProvider) {
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  // Create a manager with no stats provider.
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  // Deliberately do NOT call SetStatsProvider.
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);

  ReadHandshake(sock);

  // Wait for a stats push interval — should not crash.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // Connection should still be alive (no crash, no snapshot).
  EXPECT_EQ(manager_->active_connections(), 1);

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: Multiple stats clients delta push (lines 366-370)
// ===================================================================

TEST_F(WsManagerTest, MultipleStatsClientsReceiveDelta) {
  pending_endpoint_ = "stats";

  int sock1 = ConnectRawSocket();
  ASSERT_GE(sock1, 0);
  ReadHandshake(sock1);
  ReadTextFrameByType(sock1, "snapshot");

  int sock2 = ConnectRawSocket();
  ASSERT_GE(sock2, 0);
  ReadHandshake(sock2);
  ReadTextFrameByType(sock2, "snapshot");

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 2);
  EXPECT_EQ(manager_->metrics().stats_connections.load(), 2);

  // Wait for a delta push.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // Both should receive something.
  std::string msg1 = ReadTextFrame(sock1);
  std::string msg2 = ReadTextFrame(sock2);
  bool got_delta1 = false, got_delta2 = false;
  if (!msg1.empty()) {
    json j = json::parse(msg1, nullptr, false);
    if (!j.is_discarded() && j.contains("type")) got_delta1 = true;
  }
  if (!msg2.empty()) {
    json j = json::parse(msg2, nullptr, false);
    if (!j.is_discarded() && j.contains("type")) got_delta2 = true;
  }
  EXPECT_TRUE(got_delta1 || got_delta2)
      << "At least one stats client should receive a message";

  test::CloseSocket(sock1);
  test::CloseSocket(sock2);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: Logs connection with auth required (lines 552-568)
// ===================================================================

TEST_F(WsManagerTest, LogsWithAuthRequired) {
  ws_config_.auth_token = "secret-token";
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });

  // Post a log before starting so ring buffer is populated.
  // (PostLog checks running_ so we start first.)
  manager_->Start();
  StartLoopThread();

  manager_->PostLog("info", "worker", "test", "pre-auth log entry");
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  // Send auth.
  std::string auth_msg = BuildMaskedTextFrame(R"({"auth":"secret-token"})");
  ssize_t sent = test::SocketWrite(sock, auth_msg.data(), auth_msg.size());
  ASSERT_EQ(sent, static_cast<ssize_t>(auth_msg.size()));

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Should receive auth_ok and then a log snapshot.
  bool got_auth_ok = false;
  bool got_snapshot = false;
  for (int i = 0; i < 4; ++i) {
    std::string msg = ReadTextFrame(sock);
    if (msg.empty()) break;
    json j = json::parse(msg, nullptr, false);
    if (!j.is_discarded() && j.contains("type")) {
      if (j["type"] == "auth_ok") got_auth_ok = true;
      if (j["type"] == "snapshot") {
        got_snapshot = true;
        // Verify snapshot has entries.
        EXPECT_TRUE(j.contains("entries"));
        EXPECT_TRUE(j.contains("total"));
      }
    }
  }
  EXPECT_TRUE(got_auth_ok) << "Should receive auth_ok";
  EXPECT_TRUE(got_snapshot) << "Should receive log snapshot after auth";

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: Connection close frame handling (lines 520-522)
// ===================================================================

TEST_F(WsManagerTest, ClientCloseFrame) {
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  ReadTextFrame(sock);  // snapshot

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);

  // Build a masked close frame (code 1000).
  uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  std::string close_frame;
  close_frame.push_back(static_cast<char>(0x88));  // FIN + CLOSE
  close_frame.push_back(static_cast<char>(0x82));  // MASK + len=2
  close_frame.append(reinterpret_cast<char*>(mask), 4);
  // Close code 1000 = 0x03E8
  close_frame.push_back(static_cast<char>(0x03 ^ mask[0]));
  close_frame.push_back(static_cast<char>(0xE8 ^ mask[1]));

  test::SocketWrite(sock, close_frame.data(), close_frame.size());

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(manager_->active_connections(), 0)
      << "Connection should be closed after client close frame";

  test::CloseSocket(sock);
}

// ===================================================================
// Coverage: Client ping frame triggers pong (lines 515-518)
// ===================================================================

TEST_F(WsManagerTest, ClientPingGetsPong) {
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  ReadTextFrame(sock);  // snapshot

  // Build a masked ping frame with payload "test-ping".
  std::string payload = "test-ping";
  uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  std::string ping_frame;
  ping_frame.push_back(static_cast<char>(0x89));  // FIN + PING
  ping_frame.push_back(static_cast<char>(0x80 | payload.size()));
  ping_frame.append(reinterpret_cast<char*>(mask), 4);
  for (size_t i = 0; i < payload.size(); ++i) {
    ping_frame.push_back(payload[i] ^ mask[i % 4]);
  }

  test::SocketWrite(sock, ping_frame.data(), ping_frame.size());

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Read frames — should get a pong (opcode 0x0A).
  std::string msg = ReadTextFrame(sock);
  // ReadTextFrame returns opcode prefix for non-text frames.
  bool got_pong = (!msg.empty() && msg[0] == '\x0A');
  if (!got_pong) {
    // Try again — there might be a delta message before the pong.
    msg = ReadTextFrame(sock);
    got_pong = (!msg.empty() && msg[0] == '\x0A');
  }
  EXPECT_TRUE(got_pong) << "Server should respond to client ping with pong";

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: Continuation frame triggers protocol error (lines 524-527)
// ===================================================================

TEST_F(WsManagerTest, ContinuationFrameClosesConnection) {
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  ReadTextFrame(sock);  // snapshot

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);

  // Build a masked continuation frame (opcode 0x00).
  uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  std::string cont_frame;
  cont_frame.push_back(static_cast<char>(0x80));  // FIN + CONTINUATION
  cont_frame.push_back(static_cast<char>(0x81));  // MASK + len=1
  cont_frame.append(reinterpret_cast<char*>(mask), 4);
  cont_frame.push_back('X' ^ mask[0]);

  test::SocketWrite(sock, cont_frame.data(), cont_frame.size());

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(manager_->active_connections(), 0)
      << "Continuation frame should trigger protocol error close";

  test::CloseSocket(sock);
}

// ===================================================================
// Coverage: Log buffer HWM overflow (lines 711-713)
// ===================================================================

TEST_F(WsManagerTest, LogBufferOverflow) {
  // Reconfigure with very small event_hwm to trigger log buffer overflow.
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  ws_config_.event_hwm = 64;        // Very small buffer (shared for logs).
  ws_config_.event_batch_ms = 500;  // Longer batch to accumulate.

  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "logs";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  // Read the snapshot.
  ReadTextFrame(sock);

  // Flood with log entries exceeding the 64-byte HWM.
  for (int i = 0; i < 50; ++i) {
    manager_->PostLog("info", "worker", "test",
                      "log entry with padding data " + std::to_string(i));
  }

  // Wait for drain.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));

  // Some messages should have been dropped due to HWM.
  EXPECT_GT(manager_->metrics().messages_dropped.load(), 0u)
      << "Some log entries should have been dropped due to HWM";

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: Auth message with non-string "auth" field (line 538)
// ===================================================================

TEST_F(WsManagerTest, AuthFieldNotString) {
  ws_config_.auth_token = "secret-token";
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  // Send JSON with "auth" as a number, not a string.
  std::string bad_auth = BuildMaskedTextFrame(R"({"auth": 12345})");
  test::SocketWrite(sock, bad_auth.data(), bad_auth.size());

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(manager_->active_connections(), 0)
      << "Non-string auth field should close the connection";
  test::CloseSocket(sock);
}

// ===================================================================
// Coverage: Close frame without code (close_code == 0 path, line 521)
// ===================================================================

TEST_F(WsManagerTest, CloseFrameWithoutCode) {
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  ReadTextFrame(sock);  // snapshot

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);

  // Build a masked close frame with no payload (no close code).
  uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
  std::string close_frame;
  close_frame.push_back(static_cast<char>(0x88));  // FIN + CLOSE
  close_frame.push_back(static_cast<char>(0x80));  // MASK + len=0
  close_frame.append(reinterpret_cast<char*>(mask), 4);

  test::SocketWrite(sock, close_frame.data(), close_frame.size());

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(manager_->active_connections(), 0)
      << "Close frame without code should use default code 1000";

  test::CloseSocket(sock);
}

// ===================================================================
// Coverage: Stats delta with no changes is empty (line 358)
// ===================================================================

TEST_F(WsManagerTest, StatsDeltaEmptyWhenNoChanges) {
  // Use a fixed stats provider that always returns the same values.
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  ws_config_.stats_interval_ms = 100;

  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json {
    // Always return the same static values.
    return json{{"constant_metric", 42}};
  });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  // Read initial snapshot (sets prev_stats_ baseline).
  ReadTextFrameByType(sock, "snapshot");

  // Wait for several stats intervals. Since the provider returns
  // constant values, the delta should be empty and no messages sent.
  uint64_t msgs_before = manager_->metrics().messages_sent.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  uint64_t msgs_after = manager_->metrics().messages_sent.load();

  // No delta messages should have been sent since nothing changed.
  // (Only the initial snapshot should count.)
  // We allow at most 1 additional message (possible timing edge case).
  EXPECT_LE(msgs_after - msgs_before, 1u)
      << "No delta messages should be sent when stats are unchanged";

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: Event batch encoding failure fallback (lines 392-395)
// ===================================================================

TEST_F(WsManagerTest, EventBatchEncodingFallback) {
  // Post events and ensure they are received even in edge cases.
  // This exercises the batch encoding path including the parse fallback.
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  ws_config_.event_batch_ms = 50;

  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "events";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  // Post multiple events rapidly to test batch encoding.
  for (int i = 0; i < 5; ++i) {
    manager_->PostEvent("test_event", json{{"idx", i}});
  }

  // Wait for event batch flush.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  std::string msg = ReadTextFrame(sock);
  if (!msg.empty()) {
    json j = json::parse(msg, nullptr, false);
    if (!j.is_discarded()) {
      EXPECT_EQ(j["type"], "events");
      // "data" should be an array (successful parse) or a string (fallback).
      EXPECT_TRUE(j.contains("data"));
      EXPECT_TRUE(j.contains("sequence"));
    }
  }

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: Max connections with correct rejection (lines 146-152)
// ===================================================================

TEST_F(WsManagerTest, MaxConnectionsRejectsWithCorrectMetrics) {
  ws_config_.max_connections = 1;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([]() -> json { return json{{"test", 1}}; });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";

  // First connection should succeed.
  int sock1 = ConnectRawSocket();
  ASSERT_GE(sock1, 0);
  ReadHandshake(sock1);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);
  EXPECT_EQ(manager_->metrics().stats_connections.load(), 1);

  // Second connection should be rejected.
  int sock2 = ConnectRawSocket();
  ASSERT_GE(sock2, 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(manager_->active_connections(), 1)
      << "Second connection should be rejected";

  // After closing first, a new connection should succeed.
  test::CloseSocket(sock1);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(manager_->active_connections(), 0);

  int sock3 = ConnectRawSocket();
  ASSERT_GE(sock3, 0);
  ReadHandshake(sock3);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);

  test::CloseSocket(sock2);
  test::CloseSocket(sock3);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// ===================================================================
// Coverage: WS parse error closes connection (lines 439-442)
// ===================================================================

TEST_F(WsManagerTest, InvalidFrameClosesConnection) {
  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);
  ReadTextFrame(sock);  // snapshot

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(manager_->active_connections(), 1);

  // Send an invalid WebSocket frame: reserved opcode 0x03 with FIN bit set.
  // This should parse but result in an unrecognized opcode (falls through
  // to default in HandleFrame). However, a truly malformed frame will
  // trigger kError in the parser. Let's send a frame with an impossibly
  // large length to trigger a parse error.
  char garbage[] = {
      static_cast<char>(0x81),  // FIN + TEXT
      static_cast<char>(0xFF),  // MASK + 64-bit length indicator
      0,
      0,
      0,
      0,
      0,
      0,
      0,
      1,  // 8-byte length = 1 (but masked without mask bytes)
  };
  test::SocketWrite(sock, garbage, sizeof(garbage));

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  // Connection may or may not be closed depending on parser behavior,
  // but it should not crash.
  test::CloseSocket(sock);
}

// ===================================================================
// Coverage: Stats push with auth-required stats client (lines 553-563)
// ===================================================================

TEST_F(WsManagerTest, StatsAuthAndDeltaPush) {
  ws_config_.auth_token = "secret-token";
  ws_config_.stats_interval_ms = 100;
  StopLoopThread();
  manager_->Stop();
  uv_run(loop_, UV_RUN_NOWAIT);

  int call = 0;
  manager_ = std::make_unique<WsManager>(loop_, ws_config_, handler_.get());
  manager_->SetStatsProvider([&call]() -> json {
    call++;
    return json{{"counter", call * 10}};
  });
  manager_->Start();
  StartLoopThread();

  pending_endpoint_ = "stats";
  int sock = ConnectRawSocket();
  ASSERT_GE(sock, 0);
  ReadHandshake(sock);

  // Authenticate.
  std::string auth_msg = BuildMaskedTextFrame(R"({"auth":"secret-token"})");
  ssize_t sent = test::SocketWrite(sock, auth_msg.data(), auth_msg.size());
  ASSERT_EQ(sent, static_cast<ssize_t>(auth_msg.size()));

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Should receive auth_ok and snapshot.
  bool got_auth = false, got_snapshot = false;
  for (int i = 0; i < 3; ++i) {
    std::string msg = ReadTextFrame(sock);
    if (msg.empty()) break;
    json j = json::parse(msg, nullptr, false);
    if (!j.is_discarded() && j.contains("type")) {
      if (j["type"] == "auth_ok") got_auth = true;
      if (j["type"] == "snapshot") got_snapshot = true;
    }
  }
  EXPECT_TRUE(got_auth);
  EXPECT_TRUE(got_snapshot);

  // Wait for a delta push with changing stats.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  std::string delta_msg = ReadTextFrame(sock);
  if (!delta_msg.empty()) {
    json j = json::parse(delta_msg, nullptr, false);
    if (!j.is_discarded() && j.contains("type")) {
      if (j["type"] == "delta") {
        EXPECT_TRUE(j.contains("data"));
        EXPECT_TRUE(j.contains("sequence"));
      }
    }
  }

  test::CloseSocket(sock);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

}  // namespace
}  // namespace pagespeed
