// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024-2026 We-Amp B.V.

// PageSpeed 2.0 - Safe JSON Serialization
//
// DumpJson() is the one choke point: every JSON response and stream message
// the worker emits is serialized through it instead of calling
// nlohmann::json::dump() directly (test/src/worker:json_output_test gates
// this with a source-tree scan). A handler that fails this way answers 500
// instead of stopping the optimizer; here, the equivalent failure mode for
// a stream message is to drop that one message and keep serving.
//
// For valid input the output is byte-identical to calling dump() with the
// same arguments. For a string value nlohmann cannot losslessly encode as
// strict JSON, the offending bytes are replaced with U+FFFD instead of
// dump() throwing, so the result is always well-formed JSON.

#ifndef PAGESPEED_SRC_WORKER_JSON_DUMP_H_
#define PAGESPEED_SRC_WORKER_JSON_DUMP_H_

#include <string>

#include "nlohmann/json.hpp"

namespace pagespeed {

inline std::string DumpJson(const nlohmann::json& j, int indent = -1) {
  return j.dump(indent, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace pagespeed

#endif  // PAGESPEED_SRC_WORKER_JSON_DUMP_H_
