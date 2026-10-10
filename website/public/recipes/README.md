# Install recipes for coding agents

One file per server, each with the same five sections: prerequisites check,
install, minimal configuration, verification, rollback. Every command comes
from the documentation linked at the top of the recipe; the documentation stays
the source of truth and the recipe links to it instead of repeating it. Fenced
commands, placeholders in angle brackets, no prose beyond what a command needs.

| Server                    | Recipe                                                            | Header the verification checks |
| ------------------------- | ----------------------------------------------------------------- | ------------------------------ |
| nginx (native module)     | [nginx.md](https://modpagespeed.com/recipes/nginx.md)             | `X-Page-Speed`                 |
| Apache (native module)    | [apache.md](https://modpagespeed.com/recipes/apache.md)           | `X-Mod-Pagespeed`              |
| IIS (native module)       | [iis.md](https://modpagespeed.com/recipes/iis.md)                 | `X-Page-Speed`                 |
| ASP.NET Core (middleware) | [aspnet-core.md](https://modpagespeed.com/recipes/aspnet-core.md) | `X-PageSpeed`                  |
| Docker (reverse proxy)    | [docker.md](https://modpagespeed.com/recipes/docker.md)           | `X-PageSpeed`                  |
| Kubernetes (Helm chart)   | [helm.md](https://modpagespeed.com/recipes/helm.md)               | `X-PageSpeed`                  |

In a checkout of the repository the files are under `website/public/recipes/`.
A Claude Code skill that picks the recipe for the detected server and runs it,
verification included, is at `.claude/skills/install-modpagespeed/SKILL.md`.

## Conventions

- `<site-url>` is the scheme and host the site answers on, for example
  `http://localhost` or `https://www.example.com`.
- Linux commands run as a user with `sudo`; the IIS recipe runs in an elevated
  PowerShell.
- Stop at the first failed prerequisite and report it; do not work around it.
- A recipe is complete only when its verification step passes.
- Nothing in a recipe reports anywhere. The only network calls are the
  documented install channels: the package repository, GHCR, the Helm
  repository, nuget.org and the download page.

## The verification marker

Every verification step requests the installed site's own page with a fixed
query string:

```text
?mps-verify=agent
```

The module ignores the parameter. It is there so that whoever runs the server
can count agent-run verifications in the access log the server already writes;
the request never leaves the server it verifies.

```bash
grep -c 'mps-verify=agent' /var/log/nginx/access.log     # nginx
grep -c 'mps-verify=agent' /var/log/apache2/access.log   # Apache on Debian/Ubuntu; /var/log/httpd/ on Enterprise Linux
```

On IIS the marker is in the `cs-uri-query` field of the W3C log under
`%SystemDrive%\inetpub\logs\LogFiles\`.

## Agent-originated installs (the metric)

"Agent-originated installs" is approximated, per week, as the number of
requests for these recipe files and for `/llms.txt` whose `User-Agent` matches
a known coding agent or a non-browser client, read from the web server's access
log, plus the verification-marker hits above. Nothing is collected beyond the
access log the web server already writes.

User-Agent substrings matched, case-insensitively:

- Coding agents and their fetchers: `Claude-User`, `ClaudeBot`, `anthropic-ai`,
  `Claude-Web`, `ChatGPT-User`, `GPTBot`, `OAI-SearchBot`, `Codex`, `Cursor`,
  `Copilot`
- Non-browser clients: `python-requests`, `python-httpx`, `aiohttp`,
  `node-fetch`, `undici`, `axios`, `Go-http-client`, `curl/`, `Wget/`

One line over an nginx access log in the default `combined` format:

```bash
grep -h -E '"(GET|HEAD) /(recipes/[a-z-]+\.md|llms(-full)?\.txt)[ ?]' /var/log/nginx/access.log* \
  | grep -i -c -E 'Claude-User|ClaudeBot|anthropic-ai|Claude-Web|ChatGPT-User|GPTBot|OAI-SearchBot|Codex|Cursor|Copilot|python-requests|python-httpx|aiohttp|node-fetch|undici|axios|Go-http-client|curl/|Wget/'
```
