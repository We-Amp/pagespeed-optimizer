// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - HTTP 103 Early Hints Implementation

#include "src/nginx/early_hints.h"

#include <cstring>

namespace pagespeed {

ngx_int_t SendEarlyHints(ngx_http_request_t* r,
                         const std::vector<PreloadResource>& resources) {
  if (resources.empty()) {
    return NGX_OK;
  }

  // Build the combined Link header value.
  std::string link_value = FormatLinkHeader(resources);
  if (link_value.empty()) {
    return NGX_OK;
  }

  // Construct the raw 103 response.
  //
  // nginx does not have a built-in API for sending informational
  // responses (1xx other than 100 Continue).  We write the 103
  // response directly to the connection as raw bytes, which is the
  // standard approach for nginx modules that need 103 support.
  //
  // The response format:
  //   HTTP/1.1 103 Early Hints\r\n
  //   Link: <url>; rel=preload; as=style\r\n
  //   \r\n

  // Only send for HTTP/1.1 clients.  HTTP/1.0 doesn't support 1xx.
  // HTTP/2 and HTTP/3 have their own PUSH_PROMISE/server-push
  // mechanisms, and writing raw "HTTP/1.1 103" bytes to an h2/h3
  // connection corrupts the stream.
  if (r->http_version != NGX_HTTP_VERSION_11) {
    return NGX_OK;
  }

  // Format the response line + headers.
  std::string response = "HTTP/1.1 103 Early Hints\r\nLink: ";
  response.append(link_value);
  response.append("\r\n\r\n");

  // Allocate an nginx buffer and write directly to the connection.
  ngx_buf_t* b = ngx_create_temp_buf(r->pool, response.size());
  if (b == nullptr) {
    return NGX_ERROR;
  }

  b->last = ngx_cpymem(b->pos, response.data(), response.size());
  b->flush = 1;

  // Pool-allocate chain (never stack-allocate — if send_chain
  // performs a partial write, nginx's write event handler will
  // reference the chain later, after this function returns).
  ngx_chain_t* out =
      static_cast<ngx_chain_t*>(ngx_palloc(r->pool, sizeof(ngx_chain_t)));
  if (out == nullptr) {
    return NGX_ERROR;
  }
  out->buf = b;
  out->next = nullptr;

  // Write to the connection's send chain.  This bypasses the normal
  // filter chain since we need to send the 103 before the main
  // response headers.
  //
  // send_chain returns the *unsent* remainder of the chain.  A single call
  // can perform a partial write under socket back-pressure (returning a
  // non-NULL, non-error chain); dropping that remainder truncates the 103 and
  // corrupts the stream once the main 200 response follows.  Drive the write
  // through FlushWithRetry so the full buffer is drained before we report
  // success.  send_chain advances b->pos for the bytes it accepted, so the
  // not-yet-sent count is (b->last - b->pos).
  ngx_connection_t* c = r->connection;
  ngx_chain_t* cl = out;
  FlushStatus status = FlushWithRetry([&]() -> long {
    cl = c->send_chain(c, cl, 0);
    if (cl == NGX_CHAIN_ERROR) {
      return -1;
    }
    if (cl == nullptr) {
      return 0;  // fully sent
    }
    return static_cast<long>(b->last - b->pos);  // bytes still pending
  });
  if (status != FlushStatus::kComplete) {
    ngx_log_error(
        NGX_LOG_ERR, r->connection->log, 0,
        "pagespeed: failed to send 103 Early Hints (%s)",
        status == FlushStatus::kError ? "send error" : "write would block");
    return NGX_ERROR;
  }

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                 "pagespeed: sent 103 Early Hints (%uz bytes)",
                 response.size());

  return NGX_OK;
}

}  // namespace pagespeed
