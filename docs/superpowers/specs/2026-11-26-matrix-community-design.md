# gogo community on Matrix: design

Date: 2026-11-26

## Intent

gogo needs a place where users ask questions and talk to each other and to
the maintainer. It will be a self-hosted Matrix homeserver under the
project's own domain, not Discord. The reasons: the project owns its
community and its data, anyone can join from any Matrix client (web Element
for newcomers, terminal clients such as iamb for the maintainer), and the
protocol is open HTTP+JSON, so gogo boards could take part later.

## Goals

- Users join with one click from gogovm.dev or the README and ask questions
  without installing anything (web client via matrix.to).
- Matrix IDs look like `@name:gogovm.dev`; the community is `#gogo:gogovm.dev`.
- Federation works: people with accounts on matrix.org or other servers can
  join our rooms.
- Runs on a Raspberry Pi at home, exposed through a Cloudflare Tunnel, with
  no inbound ports opened on the home network.
- Cheap to run and easy to debug: one server binary, one tunnel daemon, both
  under systemd.

## Non-goals

- gogo boards as Matrix bots or room members. This is a possible later
  project (gogo already has TLS and `gogo/http`), not part of this one.
- Voice/video calls, bridges to Discord/IRC/Slack.
- High availability. A few hours of downtime during a Pi reboot or upgrade
  is acceptable.

## Anti-goals

- Q&A that only lives in chat. Matrix history is not indexed by search
  engines; answers that matter must be distilled into `docs/FAQ.md`,
  `docs/known-issues.md` or the book. (Process, not software: noted in the
  room topic of the help room.)
- Losing the database. The first deployment keeps it on the SD card (owner's
  decision, see below), so nightly off-device backups are mandatory, not
  optional.
- An open registration that fills the server with spam accounts.

## Constraints

- Host: `dongxu-pi5-2` (Raspberry Pi 5, 8 GB RAM, Raspberry Pi OS arm64,
  reachable over ssh as `dongxu`, passwordless). It already runs a
  Whisplay HAT status service (`whisplay-status.service`); nothing else
  listens on 8008. Network is Wi-Fi today (eth0 not connected).
- Storage: one 238 GB SD card, no SSD. Owner's decision: start with the
  database on the SD card and move it to an SSD later.
- Exposure: Cloudflare Tunnel (`cloudflared`). DNS for gogovm.dev is on
  Cloudflare. The apex `gogovm.dev` currently points directly (DNS only) at
  GitHub Pages, which serves the website and the book from
  `.github/workflows/pages.yml`.
- Cloudflare free plan: 100 MB per request body, enough for screenshots and
  logs.
- gogo repository rules: focused commits, English docs, user's git author,
  no AI attribution trailers.

## Approaches considered

| | Approach | Trade-off |
|---|---|---|
| **A (chosen)** | Native Tuwunel binary + cloudflared, both systemd services | Smallest footprint and fewest moving parts; upgrades are a manual binary swap |
| B | Docker Compose with Tuwunel and cloudflared containers | Easy to move to another machine; extra layer on a Pi, harder to debug |
| C | Synapse + PostgreSQL | Most features, but over 1 GB RAM and much more upkeep; overkill for a new community |

The owner chose A.

Registration with GitHub, two ways considered:

| | Approach | Trade-off |
|---|---|---|
| **A (chosen)** | Tuwunel's built-in GitHub SSO, password registration off | Config only, no extra service; any GitHub account can join, no allowlist or age check |
| B | Own join service (GitHub OAuth → rules → single-use registration token) | Full control over who joins; one more service to write, secure and keep running |

The owner chose A; B stays the upgrade path if spam appears.

## Design

### Names

- Server name: `gogovm.dev` (permanent: it is part of every user ID and room
  alias and cannot be changed after the first user is created).
- Homeserver host: `matrix.gogovm.dev`, served through the tunnel.
- Delegation from `gogovm.dev` to `matrix.gogovm.dev:443`:
  - `https://gogovm.dev/.well-known/matrix/server` →
    `{"m.server": "matrix.gogovm.dev:443"}`
  - `https://gogovm.dev/.well-known/matrix/client` →
    `{"m.homeserver": {"base_url": "https://matrix.gogovm.dev"}}`
    served with `Access-Control-Allow-Origin: *`.

### Serving `.well-known` from the website

The two files live in the gogo repo under `site/.well-known/matrix/` and are
published with the site:

- `make site` copies them into `build/site/.well-known/matrix/` (today it
  copies only `site/index.html`).
- `.nojekyll` is already written, so Jekyll does not drop the dot directory.
- The builder must verify that `actions/upload-pages-artifact` keeps hidden
  directories (recent versions exclude dotfiles by default) and, if not,
  make it include them.
- `tools/sitecheck` must accept extensionless JSON files under
  `.well-known` (check it does not flag them).
- The pages workflow already triggers on `site/**`.

Fallback if GitHub Pages cannot serve them correctly: add a DNS SRV record
`_matrix-fed._tcp.gogovm.dev` → `matrix.gogovm.dev:443` for federation, and
accept that clients must enter `matrix.gogovm.dev` as the homeserver.

Verify after deploy: `curl -i https://gogovm.dev/.well-known/matrix/server`
and `.../client`, then the Matrix federation tester
(federationtester.matrix.org) for `gogovm.dev`.

### The Pi

- OS: the existing Raspberry Pi OS (64-bit) on `dongxu-pi5-2`.
- Storage: database in `/srv/matrix/tuwunel` on the SD card for now. Keep
  all Matrix state under `/srv/matrix` so a later SSD migration is "stop,
  copy `/srv/matrix`, mount the SSD there, start". Builder's call: keep
  RocksDB settings write-friendly (no extra-aggressive compaction), and
  keep media uploads modest (≤ 100 MB anyway, by the Cloudflare limit).
- Network: Wi-Fi works through the tunnel; a wired connection is
  recommended later but not required.
- Tuwunel: latest release arm64 static binary in `/usr/local/bin/tuwunel`,
  config in `/etc/tuwunel/tuwunel.toml`, data in `/srv/matrix/tuwunel`,
  runs as a dedicated `tuwunel` user, systemd unit with restart on failure.
  Listens on `127.0.0.1:8008` only.
- cloudflared: installed from Cloudflare's apt repo, a named tunnel
  (credentials file, not a quick tunnel) running as a systemd service, with
  one ingress rule `matrix.gogovm.dev` → `http://127.0.0.1:8008` and a
  catch-all 404. Cloudflare DNS: `matrix` CNAME to the tunnel (proxied).
- Federation and client traffic both arrive on 443 at Cloudflare and go
  through the tunnel; nothing listens on the LAN or WAN.

### Accounts and rooms

- Registration (owner's decision): sign-up only with a GitHub account,
  through Tuwunel's built-in SSO. No open password registration.
  - A GitHub OAuth App (owner creates it, under the owner's account or a
    gogo org) with callback
    `https://matrix.gogovm.dev/_tuwunel/sso/callback/<client_id>`.
  - Tuwunel config: one `[[global.identity_provider]]` with
    `brand = "GitHub"`, `client_id`, `client_secret_file` (secret kept in a
    root-readable file under `/etc/tuwunel/`, not in the config),
    `callback_url` as above, `registration = true`, `trusted = false`
    (never true for a public provider: account takeover).
  - `[global.authentication.login] with_password = false` so `/register`
    refuses password sign-up. Password login stays possible for accounts
    that have a password (`@admin`).
  - Localpart: GitHub `login` (e.g. `@octocat:gogovm.dev`); a taken name
    falls back to Tuwunel's stable provider-derived localpart
    (`unique_id_fallbacks = true`). Display name and avatar from GitHub.
  - `@edhuang` is linked to the owner's GitHub identity with
    `!admin query oauth associate` so the owner signs in with GitHub too.
  - Must be verified on the real server before announcing (the docs are
    silent on both): a first GitHub sign-in creates an account while
    password registration is off; the localpart is the GitHub `login`.
  - Not in this version: allowlists, account-age or org checks. If spam
    appears, the planned upgrade is a small join service (GitHub OAuth +
    rules → single-use registration token via `!admin token issue --once`)
    in front, without undoing this setup.
  - People with accounts elsewhere still join over federation.
- Accounts (owner's decision): `@admin:gogovm.dev` is created first, owns
  the space and every room and holds power level 100; the owner's daily
  account `@edhuang:gogovm.dev` also gets power level 100 in every room.
- Rooms (my call, easy to extend): a space `#gogo:gogovm.dev` containing
  public rooms `#general`, `#help`, `#boards`, `#dev`. Join rule public,
  history world-readable (previewable from the web), guest access off,
  federated users may post. `#help`'s topic asks for answers worth keeping
  to go into `docs/FAQ.md`. Moderators: the two accounts above only.

### Entry points

- `matrix.to/#/#gogo:gogovm.dev` link on `site/index.html`, in `README.md`
  and in the book's introduction/FAQ ("Where can I ask questions?").
- Rollout order (my call): the repo change lands in two steps because a
  push to `main` touching `site/**` or `Makefile` deploys at once.
  Step 1: `.well-known` files + `make site` / Pages artifact changes.
  Step 2, only after the federation tester passes for `gogovm.dev`: the
  entry-point links. Each is its own focused commit with the owner as
  author and no AI trailer.

### Operations

- Backups: nightly copy of the Tuwunel database using its online backup
  feature (or stop–copy–start) to a second location; the signing key in the
  data directory is the one thing that can never be regenerated.
- Upgrades: replace the binary, restart the unit; read release notes first.
- Monitoring: none beyond systemd restart for now; the federation tester
  and `systemctl status` are the health checks.
- Restart commands and paths recorded in a short runbook.

## Left to the builder

- Exact Tuwunel version and config values beyond the ones above (cache
  sizes suitable for the Pi's RAM, media size limit ≤ 100 MB).
- Backup tool and destination details.
- Wording of the website/README/book entry points (English).

## Who does what

- Builder (via ssh to `dongxu-pi5-2`): install and configure Tuwunel,
  cloudflared, systemd units, backups, accounts, rooms; repo changes.
- Owner: `cloudflared tunnel login` (one browser authorization); create
  the GitHub OAuth App and hand over its client ID and secret; and
  choosing the off-device backup destination if the builder's proposal
  does not suit.
