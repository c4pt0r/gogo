# gogo community on Matrix: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A Tuwunel Matrix homeserver for `gogovm.dev` on `dongxu-pi5-2`, exposed through a Cloudflare Tunnel, with GitHub-only sign-up, a `#gogo` space and entry links from the website.

**Architecture:** Tuwunel (native arm64 binary, systemd) listens on `127.0.0.1:8008`; `cloudflared` (systemd, named tunnel) maps `matrix.gogovm.dev` to it. `gogovm.dev` (GitHub Pages) serves `/.well-known/matrix/{server,client}` delegating to `matrix.gogovm.dev:443`. Sign-up is Tuwunel's built-in GitHub SSO with password registration off.

**Tech Stack:** Tuwunel (latest release), cloudflared, systemd, Raspberry Pi OS arm64, GitHub Pages, curl/jq for verification and room setup.

**Spec:** `docs/superpowers/specs/2026-11-26-matrix-community-design.md`

## Global Constraints

- Server name `gogovm.dev`; homeserver host `matrix.gogovm.dev`; delegation `{"m.server": "matrix.gogovm.dev:443"}` and `{"m.homeserver": {"base_url": "https://matrix.gogovm.dev"}}` (client file with `Access-Control-Allow-Origin: *`).
- Host `dongxu-pi5-2` (ssh alias `pi5-whisplay`, user `dongxu`); do not disturb `whisplay-status.service`.
- All Matrix state under `/srv/matrix` (database `/srv/matrix/tuwunel`); config `/etc/tuwunel/tuwunel.toml`; binary `/usr/local/bin/tuwunel`; service user `tuwunel`; listen `127.0.0.1:8008` only.
- Media upload limit ≤ 100 MB (Cloudflare free plan).
- GitHub provider: `brand = "GitHub"`, `client_secret_file` under `/etc/tuwunel/` (mode 0600, owner tuwunel), `registration = true`, `trusted = false`, `unique_id_fallbacks = true`; `[global.authentication.login] with_password = false`.
- Accounts: `@admin:gogovm.dev` (password, owns everything, PL 100), `@edhuang:gogovm.dev` (PL 100, linked to the owner's GitHub). IDs are permanent: confirm both with the owner immediately before creating them.
- Rooms: space `#gogo:gogovm.dev`; rooms `#general`, `#help`, `#boards`, `#dev` (aliases `#general:gogovm.dev` etc.); join rule public, history `world_readable`, guest access `forbidden`, federation allowed.
- gogo repo commits: focused, owner as git author, no AI trailers, English text.
- Repo rollout in two commits: `.well-known` first; entry links only after the federation tester passes.
- Never start the public announcement before Task 4's verification passes.

## Review Focus

1. Pi reboot or Wi-Fi drop: both services come back without a human (`enabled`, `Restart=always`, cloudflared reconnects). Test in Task 3.
2. Power loss while on the SD card corrupts RocksDB: a restorable backup exists off the Pi. Test (restore into a scratch dir, server starts) in Task 6.
3. A GitHub user whose login is `admin` or `edhuang` signs in: they get a fallback localpart, never access to the existing account. A colliding GitHub login cannot be staged, so Task 5 verifies the guard instead: `trusted = false` in the config, and `!admin query oauth` lists the owner's link as the only association for `@edhuang`.
4. A matrix.org user opens `https://matrix.to/#/#gogo:gogovm.dev`, joins `#help` over federation and posts. Test in Task 5.
5. Element Web (app.element.io) resolves `gogovm.dev` to the right homeserver for GitHub sign-in (needs the client `.well-known` with CORS). Test in Task 4.

---

### Task 1: `.well-known` delegation on gogovm.dev

**Files:**
- Create: `site/.well-known/matrix/server`, `site/.well-known/matrix/client` (JSON, no extension, exact contents from Global Constraints)
- Modify: `Makefile:106-112` (`site:` target also copies `site/.well-known` into `$(SITE)/.well-known`)
- Modify (only if needed): `.github/workflows/pages.yml:60-62`

**Interfaces:**
- Produces: `https://gogovm.dev/.well-known/matrix/server` and `/client` used by every later task.

- [ ] **Step 1: Check the artifact action keeps dot directories.** Read `actions/upload-pages-artifact` v5.0.0's `action.yml`. If it excludes hidden files and has an input to include them, set it in `pages.yml`; if it has none, stop and report: the spec's fallback (SRV `_matrix-fed._tcp.gogovm.dev` → `0 10 443 matrix.gogovm.dev`) replaces this task's server file, and the client file needs another host (ask the owner).
- [ ] **Step 2: Write the two files and the Makefile copy.**
- [ ] **Step 3: Verify locally.** Run `make site && jq . build/site/.well-known/matrix/server build/site/.well-known/matrix/client && go run ./tools/sitecheck build/site`. Expected: both parse; sitecheck passes.
- [ ] **Step 4: Commit** (`git add site/.well-known Makefile [.github/workflows/pages.yml]`; check `git diff --cached --stat` shows only these), message `site: Matrix .well-known delegation to matrix.gogovm.dev`. Do not push yet: pushing is gated with Task 3 (Step 6 there).

### Task 2: Tuwunel on the Pi

**Files (on `dongxu-pi5-2`):**
- Create: `/usr/local/bin/tuwunel`, `/etc/tuwunel/tuwunel.toml`, `/etc/systemd/system/tuwunel.service`, `/srv/matrix/tuwunel/`

**Interfaces:**
- Produces: homeserver on `http://127.0.0.1:8008`; admin console via `tuwunel --execute` / admin room (Task 5).

- [ ] **Step 1: Install the binary.** Download the latest Tuwunel release's static aarch64 Linux binary from `github.com/matrix-construct/tuwunel/releases`, verify its checksum if published, install to `/usr/local/bin/tuwunel`; create system user `tuwunel` and `/srv/matrix/tuwunel` owned by it.
- [ ] **Step 2: Write the config.** `server_name = "gogovm.dev"`, `address = ["127.0.0.1"]`, `port = 8008`, `database_path = "/srv/matrix/tuwunel"`, `allow_federation = true`, `max_request_size` = 100 MB, `allow_registration = false` for now, `[global.well_known] client = "https://matrix.gogovm.dev"`, `server = "matrix.gogovm.dev:443"`; cache sizes for 8 GB RAM. Validate option names against the release's `tuwunel-example.toml`.
- [ ] **Step 3: systemd unit** with `User=tuwunel`, `Restart=always`, `ReadWritePaths=/srv/matrix`, hardening as in Tuwunel's packaged unit; `systemctl enable --now tuwunel`.
- [ ] **Step 4: Verify.** `curl -s http://127.0.0.1:8008/_matrix/client/versions | jq .versions` on the Pi → a non-empty list; `ss -ltn | grep 8008` → bound to 127.0.0.1 only.

### Task 3: Cloudflare Tunnel

**Files (on the Pi):** `/etc/cloudflared/config.yml`, tunnel credentials JSON under `/etc/cloudflared/`.

**Interfaces:**
- Consumes: Task 2's `127.0.0.1:8008`.
- Produces: `https://matrix.gogovm.dev`.

- [ ] **Step 1: Install** cloudflared from Cloudflare's apt repo.
- [ ] **Step 2: Owner step:** ask the owner to run `cloudflared tunnel login` on the Pi (prints a URL to open in a browser; choose zone `gogovm.dev`).
- [ ] **Step 3: Create** named tunnel `gogo-matrix`; `cloudflared tunnel route dns gogo-matrix matrix.gogovm.dev`; config ingress `matrix.gogovm.dev` → `http://127.0.0.1:8008`, then `service: http_status:404`; `cloudflared service install`; enable.
- [ ] **Step 4: Verify.** From the Mac: `curl -s https://matrix.gogovm.dev/_matrix/client/versions | jq .versions` → list; `curl -s https://matrix.gogovm.dev/_matrix/federation/v1/version` → Tuwunel server JSON.
- [ ] **Step 5: Reboot test (Review Focus 1).** `sudo reboot` the Pi; after it is back, Step 4 passes again with no manual action, and `systemctl is-active whisplay-status tuwunel cloudflared` → three `active`.
- [ ] **Step 6: Push Task 1's commit** to `main`, wait for the pages workflow (`gh run watch`), then verify `curl -si https://gogovm.dev/.well-known/matrix/client` → 200, JSON body, `access-control-allow-origin: *`; same for `server`. Then the federation tester `https://federationtester.matrix.org/api/report?server_name=gogovm.dev` → `"FederationOK": true`.

### Task 4: GitHub sign-up

**Files (on the Pi):** `/etc/tuwunel/github_client_secret`, `/etc/tuwunel/tuwunel.toml` (add provider + login section).

**Interfaces:**
- Consumes: owner-supplied GitHub OAuth App client ID and secret.
- Produces: login flows `m.login.sso` with provider brand GitHub.

- [ ] **Step 1: Owner step:** ask the owner to create a GitHub OAuth App (homepage `https://gogovm.dev`, callback `https://matrix.gogovm.dev/_tuwunel/sso/callback/<client_id>`; since the callback contains the client ID, create the app first, then edit its callback) and send the ID and secret.
- [ ] **Step 2: Configure** the `[[global.identity_provider]]` block and `with_password = false` per Global Constraints; secret file 0600 owned by `tuwunel`; restart.
- [ ] **Step 3: Verify flows.** `curl -s https://matrix.gogovm.dev/_matrix/client/v3/login | jq` → contains `m.login.sso` with an identity provider whose brand is GitHub, and `m.login.password`. `curl -s -XPOST https://matrix.gogovm.dev/_matrix/client/v3/register -d '{"username":"probe","password":"x"}'` → refused (`M_FORBIDDEN` or no password flow); no `@probe` account appears.
- [ ] **Step 4: Real sign-in (Review Focus 5).** In app.element.io choose homeserver `gogovm.dev`, "Continue with GitHub", sign in with a GitHub account that is not the owner's (ask the owner for a test account or a friend's). Never use the owner's GitHub here: it would create `@<owner-login>` before Task 5 links the owner's identity to `@edhuang`, and a deactivated Matrix ID can never be reused. Expected: account `@<github-login>:gogovm.dev` created with GitHub display name. If the account is not created or the localpart is not the GitHub login, stop and report: this is the spec's "verify before announcing" gate.

### Task 5: Accounts, space and rooms

**Files:** a setup script kept on the Pi at `/srv/matrix/setup-rooms.sh` (curl + jq against the client API as `@admin`).

**Interfaces:**
- Consumes: Task 4's working server.
- Produces: `#gogo:gogovm.dev` space and four rooms.

- [ ] **Step 1: Confirm IDs** `@admin:gogovm.dev` and `@edhuang:gogovm.dev` with the owner one last time.
- [ ] **Step 2: Create accounts** with the admin console (`!admin users create-user admin`, then `edhuang`); store the admin password in the owner's password manager (hand it to the owner, do not leave it on disk). Link `@edhuang` to the owner's GitHub with `!admin query oauth associate <client_id> @edhuang:gogovm.dev --claim login=<owner's GitHub login>` (ask the owner for the login). Verify: the owner signs in with GitHub and lands in `@edhuang`.
- [ ] **Step 3: Rooms.** The script, as `@admin`, creates the space and the four rooms with Global Constraints' settings, adds the rooms as space children, invites `@edhuang` and sets its power level to 100 in each, and sets topics; `#help`'s topic ends with "Answers worth keeping go into docs/FAQ.md." The script is idempotent (skips aliases that resolve).
- [ ] **Step 4: Verify.** `curl -s https://matrix.gogovm.dev/_matrix/client/v3/directory/room/%23gogo%3Agogovm.dev` → room_id; repeat for each room. Federation (Review Focus 4): from a matrix.org account (owner's, or ask the owner), open `https://matrix.to/#/#gogo:gogovm.dev`, join `#help`, post; message visible from `@edhuang`. Review Focus 3: `!admin query oauth` lists only the owner's link for `@edhuang`.

### Task 6: Backups and runbook

**Files (on the Pi):** `/usr/local/bin/matrix-backup`, `/etc/systemd/system/matrix-backup.{service,timer}`, `/srv/matrix/RUNBOOK.md`.

**Interfaces:**
- Consumes: backup destination confirmed by the owner (proposal: the owner's Mac or workstation over ssh, a directory such as `~/backups/gogo-matrix`).

- [ ] **Step 1: Ask the owner for the destination.**
- [ ] **Step 2: Backup script.** Use Tuwunel's online database backup (`database_backup_path = "/srv/matrix/backup"`, triggered with the admin command `server backup-database` via `tuwunel --execute` or the admin room; keep `database_backups_to_keep = 3`), then `rsync -a /srv/matrix/backup/ /etc/tuwunel/ <dest>` (includes the signing key inside the DB and the config/secret). Timer daily at 04:00, `Persistent=true`.
- [ ] **Step 3: Verify (Review Focus 2).** Run the service once: new backup on the Pi and at the destination. Restore test: on the Pi, start a second Tuwunel with a copy of the latest backup as `database_path`, a scratch config on `127.0.0.1:8009`, federation off; `curl 127.0.0.1:8009/_matrix/client/versions` answers and `!admin users list-users` (via `--execute`) shows `admin` and `edhuang`; then stop it and delete the scratch copy.
- [ ] **Step 4: Runbook** `/srv/matrix/RUNBOOK.md`: paths, start/stop/status, upgrade steps (swap binary, restart, check versions), how to deactivate a spam account, how to link a GitHub identity, backup/restore steps, SSD migration steps from the spec.

### Task 7: Entry links (after Task 3 Step 6 passes)

**Files:**
- Modify: `site/index.html` (a "Community" link to `https://matrix.to/#/#gogo:gogovm.dev`, next to the existing GitHub/book links)
- Modify: `README.md` (near the docs list at line ~231: "Questions and discussion: the `#gogo:gogovm.dev` Matrix space")
- Modify: `docs/FAQ.md` (new question "Where can I ask questions?": Matrix link, sign in with GitHub or any Matrix account, issues for bugs)

- [ ] **Step 1: Edit the three files** (English).
- [ ] **Step 2: Verify.** `make site-check` passes; the link opens matrix.to and shows the space.
- [ ] **Step 3: Commit** (only these three files), message `site, README, FAQ: link the #gogo Matrix space`; push; confirm the pages run deploys.
