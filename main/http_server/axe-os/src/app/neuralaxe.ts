/**
 * NeuralAxe OS product identity — read-only build metadata.
 *
 * NeuralAxe OS is based on the open-source ESP-Miner and AxeOS projects
 * (GPL-3.0). Upstream firmware/AxeOS version semantics are unchanged;
 * these values are additive product identity only.
 */
export const NEURALAXE = Object.freeze({
  productName: 'NeuralAxe OS',
  productVersion: '0.1.0-dev',
  buildChannel: 'development',
  vendor: 'NeuralShield',
  upstreamProject: 'ESP-Miner / AxeOS',
  upstreamVersion: 'v2.14.2',
  targetBoard: '601',
  targetDevice: 'Gamma',
  targetAsic: 'BM1370',
  attribution: 'NeuralAxe OS is based on the open-source ESP-Miner and AxeOS projects.',
} as const);
