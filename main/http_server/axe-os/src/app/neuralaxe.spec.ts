import { NEURALAXE } from './neuralaxe';

describe('NEURALAXE product identity', () => {
  it('should declare the NeuralAxe OS product identity', () => {
    expect(NEURALAXE.productName).toBe('NeuralAxe OS');
    expect(NEURALAXE.productVersion).toBe('0.1.0-dev');
    expect(NEURALAXE.buildChannel).toBe('development');
    expect(NEURALAXE.vendor).toBe('NeuralShield');
  });

  it('should declare the build target', () => {
    expect(NEURALAXE.targetBoard).toBe('601');
    expect(NEURALAXE.targetDevice).toBe('Gamma');
    expect(NEURALAXE.targetAsic).toBe('BM1370');
  });

  it('should attribute the upstream projects and keep versions separate', () => {
    expect(NEURALAXE.upstreamProject).toBe('ESP-Miner / AxeOS');
    expect(NEURALAXE.upstreamVersion).toBe('v2.14.2');
    expect(NEURALAXE.attribution)
      .toContain('based on the open-source ESP-Miner and AxeOS projects');
    // Product identity must never masquerade as the upstream version.
    expect(NEURALAXE.productVersion).not.toBe(NEURALAXE.upstreamVersion);
  });

  it('should be frozen (read-only metadata)', () => {
    expect(Object.isFrozen(NEURALAXE)).toBeTrue();
  });

  it('should pin the update channel to the NeuralAxe repository', () => {
    expect(NEURALAXE.updateRepository).toBe('AegisAI-Dev/NeuralAxe-OS');
    expect(NEURALAXE.updateRepository).not.toContain('bitaxeorg');
  });
});
