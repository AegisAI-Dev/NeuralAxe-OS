/**
 * Command Deck compact Pool Strategy glance (Phase 2M, Stage 12).
 *
 * Pure derivation of the small deck card from the chain context, the local
 * switch history and restore availability. It never exposes a credential and
 * never asserts a chain that the context could not verify.
 */

import { ChainContext } from './pool-chain';
import { PoolSwitchRecord } from './pool-history';
import { RecoveryResult, SwitchState } from './pool-switch-machine';

export interface PoolDeckLastSwitch {
  targetName: string | null;
  sourceChain: string;
  targetChain: string;
  state: SwitchState;
  verified: boolean;
  rollbackResult: RecoveryResult | null;
  atMs: number;
}

export interface PoolDeckGlance {
  chainLabel: string;
  chainShort: string;
  chainSeverity: 'ok' | 'info' | 'neutral';
  profileName: string | null;
  verified: boolean;
  detail: string;
  lastSwitch: PoolDeckLastSwitch | null;
  restoreAvailable: boolean;
  switching: boolean;
  /** True only when a BCH-labelled profile is verified as active. */
  suppressBitcoinMatch: boolean;
}

function chainSeverity(context: ChainContext): 'ok' | 'info' | 'neutral' {
  if (!context.labelled) return 'neutral';
  return context.chain === 'BTC' ? 'ok' : context.chain === 'BCH' ? 'info' : 'neutral';
}

export function poolDeckGlance(input: {
  context: ChainContext;
  history: PoolSwitchRecord[];
  restoreAvailable: boolean;
  switchActive: boolean;
}): PoolDeckGlance {
  const ctx = input.context;
  const last = Array.isArray(input.history) && input.history.length ? input.history[0] : null;
  return {
    chainLabel: ctx.label,
    chainShort: ctx.short,
    chainSeverity: chainSeverity(ctx),
    profileName: ctx.profileName,
    verified: ctx.verified,
    detail: ctx.detail,
    lastSwitch: last ? {
      targetName: last.targetProfileName,
      sourceChain: last.sourceChain,
      targetChain: last.targetChain,
      state: last.finalState,
      verified: last.switchVerified,
      rollbackResult: last.rollbackResult,
      atMs: last.finishedAt,
    } : null,
    restoreAvailable: input.restoreAvailable,
    switching: input.switchActive,
    suppressBitcoinMatch: ctx.labelled && ctx.chain === 'BCH',
  };
}
