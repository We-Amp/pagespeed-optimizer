# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2024-2026 We-Amp B.V.

"""Settings check for the repository rules. Not used by the build."""

from flask import Flask

app = Flask(__name__)


@app.route("/")
def index():
    return "ok"


if __name__ == "__main__":
    app.run(debug=True)
