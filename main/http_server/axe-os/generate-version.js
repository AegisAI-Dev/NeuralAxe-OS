const fs = require('fs');
const path = require('path');
const { execSync } = require('child_process');

/*
 * Writes the web revision that ends up inside www.bin as version.txt, which the
 * device serves as axeOSVersion and compares — by EXACT STRING EQUALITY —
 * against the firmware's esp_app_desc_t version.
 *
 * The two sides must therefore agree character for character. They previously
 * did not: both ran `git describe` without an explicit --abbrev, and git's
 * default ("auto") differs between git versions and hosts. The firmware builds
 * in the ESP-IDF container and this script runs on the host, so the same commit
 * produced v2.14.2-70-g34a51508 (8 hex) here and v2.14.2-70-g34a5150 (7 hex)
 * there, and a real device correctly reported BOOT PAIR MISMATCH.
 *
 * 1. NX_CANONICAL_REVISION, when set, is used verbatim. Packaging helpers set
 *    it to the one canonical revision they also pass to the firmware build as
 *    -DPROJECT_VER, so neither side derives its own.
 * 2. Otherwise the fallback pins the abbreviation EXPLICITLY, so a plain
 *    `idf.py build` is deterministic too rather than depending on the host.
 *
 * See tools/pilot/canonical_revision.py for the shared contract.
 */
const CANONICAL_DESCRIBE = 'git describe --tags --long --always --abbrev=8';

const supplied = (process.env.NX_CANONICAL_REVISION || '').trim();
const version = supplied || execSync(CANONICAL_DESCRIBE).toString().trim();

if (!version) {
  console.error('generate-version.js: refusing to write an empty version.txt');
  process.exit(1);
}

const outputDir = path.join(__dirname, 'dist', 'axe-os');
if (!fs.existsSync(outputDir)) {
  console.error(`generate-version.js: ${outputDir} does not exist; build the web UI first`);
  process.exit(1);
}

const outputPath = path.join(outputDir, 'version.txt');
fs.writeFileSync(outputPath, version);

console.log(`Generated ${outputPath} with version ${version}` +
            (supplied ? ' (supplied canonical revision)' : ' (derived, pinned --abbrev=8)'));
