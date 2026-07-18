/**
 * OTA upload filename classification (Phase 2H.1).
 *
 * The release exporter stages board-identified names
 * (NeuralAxe-OS-v…-Gamma-601-www.bin / …-ota.bin) while the uploader accepted
 * only the literal legacy names, forcing manual renames. This module accepts
 * both name families and — more importantly — explicitly rejects every image
 * type that must never travel through the OTA endpoints (factory/merged
 * full-flash images, bootloader, partition table, otadata, provisioning
 * config). Pure and fully tested; the backend OTA mechanics are unchanged.
 */

/** Which uploader the user handed the file to. */
export type UpdateUploadKind = 'www' | 'firmware';

export type DetectedFileType =
  | 'www'
  | 'firmware'
  | 'factory'
  | 'merged'
  | 'bootloader'
  | 'partition-table'
  | 'ota-data'
  | 'config'
  | 'unknown';

export interface UpdateFileCheck {
  accepted: boolean;
  detectedType: DetectedFileType;
  /** Short human-readable name of the detected type. */
  typeLabel: string;
  /** Why the file was accepted or rejected — always shown to the user. */
  reason: string;
}

const TYPE_LABELS: { [key in DetectedFileType]: string } = {
  'www': 'Web interface update (www)',
  'firmware': 'Firmware update (OTA application)',
  'factory': 'Factory full-flash image',
  'merged': 'Merged full-flash image',
  'bootloader': 'Bootloader image',
  'partition-table': 'Partition table image',
  'ota-data': 'OTA data partition image',
  'config': 'Board provisioning config',
  'unknown': 'Unrecognized file',
};

/**
 * Classify a filename. Dangerous classifications intentionally take
 * precedence over the acceptance suffixes, so a name like
 * "factory-www.bin" can never slip through as a web update.
 */
export function detectUpdateFileType(filename: string): DetectedFileType {
  const name = (filename || '').trim().toLowerCase();
  if (!name) {
    return 'unknown';
  }
  if (name.endsWith('.cvs') || name.endsWith('.csv')) {
    return 'config';
  }
  if (!name.endsWith('.bin')) {
    return 'unknown';
  }
  // Never-upload types first (full-flash / USB-recovery images).
  if (name.includes('ota_data') || name.includes('otadata')) {
    return 'ota-data';
  }
  if (name.includes('factory')) {
    return 'factory';
  }
  if (name.includes('merged')) {
    return 'merged';
  }
  if (name.includes('bootloader')) {
    return 'bootloader';
  }
  if (name.includes('partition')) {
    return 'partition-table';
  }
  // Accepted families: exact legacy names plus the NeuralAxe export suffixes.
  if (name === 'www.bin' || name.endsWith('-www.bin')) {
    return 'www';
  }
  if (name === 'esp-miner.bin' || name.endsWith('-ota.bin')) {
    return 'firmware';
  }
  return 'unknown';
}

const REJECT_REASONS: { [key in Exclude<DetectedFileType, 'www' | 'firmware'>]: string } = {
  'factory': 'Factory images are full-flash USB-recovery images (esptool/bitaxetool at offset 0x0). Flashing one over OTA is not possible from this page.',
  'merged': 'Merged full-flash images are for USB recovery with esptool and cannot be installed over OTA.',
  'bootloader': 'Bootloader images cannot be installed over OTA — USB recovery only.',
  'partition-table': 'Partition-table images cannot be installed over OTA — USB recovery only.',
  'ota-data': 'The OTA data partition image is part of a full USB flash and cannot be uploaded here.',
  'config': 'Board provisioning configs are applied with bitaxetool over USB, not through this page.',
  'unknown': 'Unrecognized file name. Expected www.bin / *-www.bin for the web interface, or esp-miner.bin / *-ota.bin for firmware.',
};

/** Validate a selected file against the uploader it was handed to. */
export function checkUpdateFile(filename: string, kind: UpdateUploadKind): UpdateFileCheck {
  const detectedType = detectUpdateFileType(filename);
  const typeLabel = TYPE_LABELS[detectedType];

  if (detectedType === kind || (detectedType === 'www' && kind === 'www') || (detectedType === 'firmware' && kind === 'firmware')) {
    return {
      accepted: true,
      detectedType,
      typeLabel,
      reason: detectedType === 'www'
        ? 'Web interface image — installs to the www partition only.'
        : 'Firmware image — installs to the standby OTA partition; the device restarts to finish.',
    };
  }

  // Right family, wrong uploader: say exactly where it belongs.
  if (detectedType === 'www' && kind === 'firmware') {
    return { accepted: false, detectedType, typeLabel, reason: 'This is a web-interface image — use the "Install Web Interface" uploader instead.' };
  }
  if (detectedType === 'firmware' && kind === 'www') {
    return { accepted: false, detectedType, typeLabel, reason: 'This is a firmware image — use the "Install Firmware" uploader instead.' };
  }

  return {
    accepted: false,
    detectedType,
    typeLabel,
    reason: REJECT_REASONS[detectedType as Exclude<DetectedFileType, 'www' | 'firmware'>],
  };
}
