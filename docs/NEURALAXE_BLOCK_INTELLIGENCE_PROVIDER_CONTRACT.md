# NeuralAxe OS — Block Intelligence Provider & Attribution Contract (Phase 2L)

> Reference for the Bitcoin Block Intelligence subsystem. This is **not** the
> Phase 2L final report (that is written in Phase B). It documents the attribution
> model and the exact outbound request contract, and is kept in sync with the
> code and tests.

## 1. Attribution certainty model

Public providers supply **evidence, not proof**. The strongest state the
currently-implemented public providers (mempool.space, Esplora) ever produce is
**Provider-reported**. `Confirmed` is **reserved** for a future source with
direct authoritative provenance (an explicit pool, Stratum or node contract) and
is **never** emitted for public-provider data.

| State | Meaning | Example wording | Severity (fixed, accent-independent) |
|---|---|---|---|
| `confirmed` | **Reserved** — future authoritative source only | (never shown for public data) | ok |
| `provider-reported` | The block-data provider attributes this block to a pool (`extras.pool`) | “Provider attributes this block to Foundry USA” | ok |
| `strong` | Coinbase tag clearly names a known pool (name/label token) | “Strong coinbase-tag match: AntPool” | ok |
| `probable` | Likely pool from alias/domain evidence, **or** downgraded conflicting evidence | “Likely Foundry USA based on coinbase alias/domain evidence” | info |
| `unknown` | Coinbase present but no pool identified | “Unknown pool” | neutral |
| `unattributed` | No coinbase/evidence available | “Unattributed” | neutral |

Derivation rules (`attribution.ts` → `deriveAttribution`):

- provider `extras.pool` present, no conflicting coinbase → **provider-reported** (canonical pool name; corroboration is noted in the reason but never upgrades the state);
- provider `extras.pool` present **and** the coinbase tag names a *different* known pool → downgraded to **probable** (conflict);
- no provider pool, coinbase tag matches a pool **name token** → **strong**;
- no provider pool, coinbase tag matches only a pool **domain/alias** → **probable**;
- no provider pool, coinbase present but unmatched → **unknown**;
- no coinbase/evidence → **unattributed**.

Honesty guarantees (enforced by tests):

- the public-provider adapters never emit `confirmed`;
- `unknown` and `unattributed` are distinct and never guessed away;
- no reason/label ever asserts that this individual NeuralAxe device found a block;
- configured-pool matching (`Active/Fallback pool match`) is a **pool-identity
  alignment** only — it never implies the local device produced the block.

## 2. Outbound request contract

Every external request is a **bare GET** built solely from the static provider
descriptor plus (for detail) a percent-encoded block hash. There is no request
body, no `Authorization` header, and no query string. The operator's browser
makes the request; the device never contacts the provider.

| Purpose | Method | URL |
|---|---|---|
| mempool.space recent blocks | GET | `https://mempool.space/api/v1/blocks` |
| mempool.space block detail | GET | `https://mempool.space/api/v1/block/{hash}` |
| Esplora recent blocks | GET | `https://blockstream.info/api/blocks` |
| Esplora block detail | GET | `https://blockstream.info/api/block/{hash}` |

`{hash}` is a percent-encoded block hash and is the **only** variable component;
the URL builders (`provider-request.ts`) cannot embed any other value, so even a
hostile hash cannot inject a query parameter. Provider fallback (mempool →
Esplora) uses the plain Esplora URL with no sensitive context appended.

## 3. Privacy contract

Configured-pool matching happens **locally in the browser, only after** public
block data is received. The following are **never** transmitted to a block-data
provider, and never appear in any outbound URL or query:

- wallet, worker, pool password;
- SSID, Wi-Fi password;
- hostname, miner IP, device serial;
- hashrate, temperature, tuning, Fleet data;
- the configured active/fallback pool values.

Only public Bitcoin block data is requested. CORS was validated empirically for
both providers (200 + JSON from a foreign browser origin), so no firmware proxy
is required.

## 4. Future local-node provider

The provider abstraction (`BlockProviderDescriptor` + pure `normalize*`) is
designed so a future LAN/local provider (Bitcoin Core RPC, Electrs, Esplora or a
NeuralAxe Node) can be added with priority `local → lan → public → cache →
offline`. The Esplora adapter already speaks the schema a self-hosted instance
would serve. No credentials are stored in the frontend.
