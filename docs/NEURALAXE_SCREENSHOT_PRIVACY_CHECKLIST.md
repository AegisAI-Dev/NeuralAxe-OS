# NeuralAxe OS — Screenshot & Public-Documentation Privacy Checklist

Any screenshot or screen recording that leaves the owner's machine (bug reports,
documentation, release notes, social posts) must pass this checklist. The
production UI always shows the owner real values — privacy here is a *capture*
discipline plus the built-in sensitive-data mode, never a configuration change.

## 1. Before capturing

- [ ] Turn **sensitive-data hiding ON** (the eye icon in the top bar). This is
      the default state; it masks every element tagged `[sensitive-data]`:
      hostname, Wi-Fi SSID, IPv4/IPv6, MAC address, pool endpoints, worker /
      wallet identifiers (Command Deck, Device Status, Pools, Fleet, Network).
- [ ] Prefer capturing from the **development server** (mock data) for
      documentation — every value there is synthetic.
- [ ] For the **Logs** page: press *Clear Logs* first, or review the visible
      buffer line by line — serial output can contain the SSID, IP addresses
      and pool hostnames. Never capture a scrollback you have not read.
- [ ] Close browser dev-tools, other tabs and OS overlays (they may leak
      hostnames or account names in window chrome).

## 2. What must never appear

- Wi-Fi SSID or password (any form)
- LAN IPv4 / IPv6 addresses or MAC addresses
- Wallet addresses / worker identifiers (full or partially recognizable)
- Pool passwords, TLS certificates, SV2 authority keys
- Serial numbers, private dump filenames, machine-local paths (`C:\Users\…`)

## 3. After capturing

- [ ] Zoom to 100% and read every visible string — including tooltips caught
      mid-hover, browser autocomplete, and background toasts.
- [ ] Check the browser URL/tab strip if included in the frame.
- [ ] Run the DOM-text privacy scan (release pipeline) or grep exported page
      text for MAC (`..:..:..:..:..:..`), IPv4, `bc1`/`1`/`3` wallet prefixes.
- [ ] Store originals outside the repository; only sanitized copies may be
      published. `tools/release/export_release.py --screenshot-scan <report>`
      refuses an RC export without a clean scan report.

## 4. If something leaked

Treat the capture as compromised: delete it everywhere it was uploaded,
rotate the exposed secret where applicable (Wi-Fi password, pool password),
and re-capture with this checklist.
