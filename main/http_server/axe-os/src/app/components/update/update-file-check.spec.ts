import { checkUpdateFile, detectUpdateFileType } from './update-file-check';

describe('update-file-check (Phase 2H.1 OTA filename compatibility)', () => {
  describe('web uploader acceptance', () => {
    it('accepts the legacy www.bin', () => {
      const check = checkUpdateFile('www.bin', 'www');
      expect(check.accepted).toBeTrue();
      expect(check.detectedType).toBe('www');
    });

    it('accepts the NeuralAxe release-export www name', () => {
      const check = checkUpdateFile('NeuralAxe-OS-v0.1.0-dev-Gamma-601-www.bin', 'www');
      expect(check.accepted).toBeTrue();
      expect(check.detectedType).toBe('www');
      expect(check.reason).toContain('www partition');
    });
  });

  describe('firmware uploader acceptance', () => {
    it('accepts the legacy esp-miner.bin', () => {
      const check = checkUpdateFile('esp-miner.bin', 'firmware');
      expect(check.accepted).toBeTrue();
      expect(check.detectedType).toBe('firmware');
    });

    it('accepts the NeuralAxe release-export ota name', () => {
      const check = checkUpdateFile('NeuralAxe-OS-v0.1.0-dev-Gamma-601-ota.bin', 'firmware');
      expect(check.accepted).toBeTrue();
      expect(check.detectedType).toBe('firmware');
      expect(check.reason).toContain('standby OTA partition');
    });
  });

  describe('dangerous image rejection (both uploaders)', () => {
    const dangerous: Array<[string, string]> = [
      ['factory.bin', 'factory'],
      ['NeuralAxe-OS-v0.1.0-dev-Gamma-601-factory.bin', 'factory'],
      ['esp-miner-factory-v2.14.2.bin', 'factory'],
      ['esp-miner-merged.bin', 'merged'],
      ['bootloader.bin', 'bootloader'],
      ['partition-table.bin', 'partition-table'],
      ['ota_data_initial.bin', 'ota-data'],
      ['config-601.cvs', 'config'],
    ];

    for (const [name, expectedType] of dangerous) {
      it(`rejects ${name} from both uploaders as ${expectedType}`, () => {
        for (const kind of ['www', 'firmware'] as const) {
          const check = checkUpdateFile(name, kind);
          expect(check.accepted).withContext(`${name} via ${kind}`).toBeFalse();
          expect(check.detectedType).withContext(name).toBe(expectedType as any);
          expect(check.reason.length).toBeGreaterThan(10);
        }
      });
    }

    it('dangerous markers take precedence over the acceptance suffixes', () => {
      // A hostile/confused name must never classify as installable.
      expect(detectUpdateFileType('factory-www.bin')).toBe('factory');
      expect(detectUpdateFileType('bootloader-ota.bin')).toBe('bootloader');
      expect(detectUpdateFileType('merged-www.bin')).toBe('merged');
    });
  });

  describe('wrong-uploader cross rejection', () => {
    it('rejects a firmware image handed to the web uploader, naming the right place', () => {
      const check = checkUpdateFile('NeuralAxe-OS-v0.1.0-dev-Gamma-601-ota.bin', 'www');
      expect(check.accepted).toBeFalse();
      expect(check.detectedType).toBe('firmware');
      expect(check.reason).toContain('Install Firmware');
    });

    it('rejects a web image handed to the firmware uploader, naming the right place', () => {
      const check = checkUpdateFile('www.bin', 'firmware');
      expect(check.accepted).toBeFalse();
      expect(check.detectedType).toBe('www');
      expect(check.reason).toContain('Install Web Interface');
    });
  });

  describe('unknown and malformed names', () => {
    it('rejects arbitrary bin files with an explanatory reason', () => {
      const check = checkUpdateFile('arbitrary.bin', 'firmware');
      expect(check.accepted).toBeFalse();
      expect(check.detectedType).toBe('unknown');
      expect(check.reason).toContain('esp-miner.bin');
    });

    it('rejects non-bin files, empty names, and case variants safely', () => {
      expect(checkUpdateFile('readme.txt', 'www').accepted).toBeFalse();
      expect(checkUpdateFile('', 'www').accepted).toBeFalse();
      expect(checkUpdateFile('  ', 'firmware').accepted).toBeFalse();
      // classification is case-insensitive
      expect(detectUpdateFileType('WWW.BIN')).toBe('www');
      expect(detectUpdateFileType('NeuralAxe-OS-v0.1.0-DEV-Gamma-601-OTA.bin')).toBe('firmware');
      expect(detectUpdateFileType('ESP-Miner-FACTORY.bin')).toBe('factory');
    });
  });
});
