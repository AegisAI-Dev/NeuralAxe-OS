/**
 * Outbound provider request contract (Phase 2L, Stage 11).
 *
 * The ONLY URLs the app ever contacts for block data are built here, from the
 * static provider descriptor plus (for detail) a block hash. There is NO query
 * string and NO way to inject device/config values: these builders do not even
 * accept configured-pool, wallet, worker, telemetry or Fleet data. Requests are
 * always GET, with no body and no Authorization header (see the service).
 *
 * Documented contract:
 *   - mempool.space recent blocks : GET https://mempool.space/api/v1/blocks
 *   - mempool.space block detail   : GET https://mempool.space/api/v1/block/{hash}
 *   - Esplora recent blocks        : GET https://blockstream.info/api/blocks
 *   - Esplora block detail         : GET https://blockstream.info/api/block/{hash}
 * where {hash} is a percent-encoded 64-hex block hash and nothing else.
 */

import { BlockProviderDescriptor } from './block-intelligence.model';

/** Recent-blocks URL for a provider (no query string, ever). */
export function providerBlocksUrl(descriptor: BlockProviderDescriptor): string {
  return descriptor.baseUrl + descriptor.recentBlocksPath;
}

/**
 * Block-detail URL for a provider. Only the block hash is substituted, and it is
 * percent-encoded so a hash is the sole variable component. No query string.
 */
export function providerBlockDetailUrl(descriptor: BlockProviderDescriptor, hash: string): string {
  return descriptor.baseUrl + descriptor.blockDetailPath.replace(':hash', encodeURIComponent(hash));
}
