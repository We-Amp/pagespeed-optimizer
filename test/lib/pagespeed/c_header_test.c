// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

/* The public header compiled as C (C11, pedantic, warnings as errors), the
 * language floor the header states: a C embedder that includes it must
 * build. It checks a few layouts with C's own tools, so a header that is
 * only valid C++ fails here. */

#include <stddef.h>

#include "lib/pagespeed/pagespeed.h"

_Static_assert(offsetof(ps_html_serve_request_t, markdown_encoding) ==
                   offsetof(ps_html_serve_request_t, reserved),
               "markdown_encoding and reserved are one word");
_Static_assert(sizeof(size_t) != 8 || sizeof(ps_html_serve_request_t) == 88,
               "the serve request is 88 bytes on 64-bit targets");
_Static_assert(sizeof(size_t) != 8 || sizeof(ps_html_serve_verdict_t) == 72,
               "the serve verdict is 72 bytes on 64-bit targets");
_Static_assert(offsetof(ps_html_serve_verdict_t, reserved_1_16) +
                       sizeof(int) ==
                   sizeof(ps_html_serve_verdict_t),
               "the verdict has no implicit tail padding");

int main(void) {
  ps_html_serve_request_t request = {0};
  request.markdown_encoding = PS_MARKDOWN_ENCODING_BR;
  return request.reserved == PS_MARKDOWN_ENCODING_BR ? 0 : 1;
}
