// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.
//
// Windows: the notification client connects at identification level.

#include <gtest/gtest.h>

#ifdef _WIN32
#include <windows.h>

#include <string>
#include <thread>

#include "src/proto/notification_sender.h"
#include "src/proto/worker_ipc.h"
#endif

namespace pagespeed {
namespace {

#ifdef _WIN32

TEST(NotificationSenderWinTest, ConnectsAtIdentificationLevel) {
  const std::string name =
      "pagespeed-identity-level-test-" + std::to_string(GetCurrentProcessId());
  const std::string full = "\\\\.\\pipe\\" + name;
  HANDLE server = CreateNamedPipeA(
      full.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_WAIT,
      /*nMaxInstances=*/1, /*nOutBufferSize=*/4096, /*nInBufferSize=*/4096,
      /*nDefaultTimeOut=*/0, /*lpSecurityAttributes=*/nullptr);
  ASSERT_NE(server, INVALID_HANDLE_VALUE) << GetLastError();

  CacheNotification notification{};
  notification.url = "http://example.test/a.css";
  notification.hostname = "example.test";
  notification.scheme = "http";

  // The persistent sender keeps its handle open after the write, so the
  // client is still connected when the server impersonates it.
  NotificationSendResult sent;
  std::thread client(
      [&] { sent = SendNotificationPersistent(name, notification); });

  const BOOL connected = ConnectNamedPipe(server, nullptr);
  const DWORD connect_error = connected ? 0 : GetLastError();
  char buffer[4096];
  DWORD read = 0;
  // A server may impersonate only after it has read from the pipe.
  const BOOL got_data =
      ReadFile(server, buffer, sizeof(buffer), &read, nullptr);
  // Joined before any assertion, so a failure fails the test rather than
  // ending the process with a joinable thread.
  client.join();
  ASSERT_TRUE(connected || connect_error == ERROR_PIPE_CONNECTED);
  ASSERT_TRUE(got_data);
  ASSERT_TRUE(sent.success) << sent.error_message;

  ASSERT_TRUE(ImpersonateNamedPipeClient(server)) << GetLastError();
  HANDLE token = nullptr;
  const BOOL opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY,
                                      /*OpenAsSelf=*/TRUE, &token);
  SECURITY_IMPERSONATION_LEVEL level = SecurityAnonymous;
  DWORD length = 0;
  const BOOL queried =
      opened && GetTokenInformation(token, TokenImpersonationLevel, &level,
                                    sizeof(level), &length);
  RevertToSelf();
  if (token != nullptr) CloseHandle(token);
  ClosePersistentConnection();
  CloseHandle(server);

  ASSERT_TRUE(opened);
  ASSERT_TRUE(queried);
  EXPECT_EQ(level, SecurityIdentification) << "got " << level;
}

#else

TEST(NotificationSenderWinTest, WindowsOnly) {
  GTEST_SKIP() << "named pipe identity levels are a Windows concept";
}

#endif

}  // namespace
}  // namespace pagespeed
