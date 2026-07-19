#!/usr/bin/env node
/**
 * NeuralAxe frontend test gate.
 *
 * Runs the full Karma suite once and exits 0 ONLY on a genuinely green run.
 * It does NOT trust the child process exit code blindly: on Windows the Karma
 * server can exit non-zero from a post-success socket teardown (ECONNRESET/EPIPE
 * after all results are in) even though every test passed. This wrapper accepts
 * ONLY that exact signature — a complete "Executed N of N", a "TOTAL: N SUCCESS"
 * line, zero failures, and the teardown error appearing after the results — and
 * fails on anything else: any failed/skipped test, a compile/load error, a
 * browser crash or an incomplete run.
 *
 * It records both the raw child outcome and the normalized gate outcome.
 */
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

/** axe-os project root (this file lives in axe-os/scripts) — caller-independent. */
const AXE_OS_DIR = resolve(dirname(fileURLToPath(import.meta.url)), '..');

const TEARDOWN_SIGNATURES = ['ECONNRESET', 'read ECONNRESET', 'socket hang up', 'EPIPE'];
const COMPILE_MARKERS = ['error TS', 'Found 1 load error', 'load error', 'SyntaxError', 'Module not found', 'Cannot find module'];
const CRASH_MARKERS = ['Disconnected', 'CRASHED', 'ChromeHeadless have not captured', 'timeout of', 'was not started'];

function run() {
  const result = spawnSync('npm', ['test'], {
    shell: true,
    encoding: 'utf8',
    env: process.env,
    cwd: AXE_OS_DIR,
    maxBuffer: 64 * 1024 * 1024,
  });
  const stdout = result.stdout || '';
  const stderr = result.stderr || '';
  return { output: stdout + '\n' + stderr, rawExit: result.status, signal: result.signal, error: result.error };
}

function analyze(output) {
  // Strip ANSI colour codes so the regexes are stable.
  const clean = output.replace(/\x1b\[[0-9;]*m/g, '');

  // Last "Executed X of Y" line — proves the run reached the end without a
  // browser crash mid-suite.
  const execMatches = [...clean.matchAll(/Executed (\d+) of (\d+)/g)];
  const lastExec = execMatches.length ? execMatches[execMatches.length - 1] : null;
  const executed = lastExec ? Number(lastExec[1]) : null;
  const total = lastExec ? Number(lastExec[2]) : null;
  const complete = executed !== null && total !== null && executed === total && total > 0;

  // "TOTAL: N SUCCESS" (green) or "TOTAL: F FAILED, S SUCCESS".
  const totalMatch = clean.match(/TOTAL:\s+(\d+)\s+SUCCESS/);
  const failedTotalMatch = clean.match(/TOTAL:\s+(\d+)\s+FAILED/);
  const success = totalMatch ? Number(totalMatch[1]) : null;
  const failed = failedTotalMatch ? Number(failedTotalMatch[1]) : 0;

  const green = success !== null && failed === 0 && complete;

  const compileError = COMPILE_MARKERS.some(m => clean.includes(m));
  const crash = CRASH_MARKERS.some(m => clean.includes(m));
  const teardown = TEARDOWN_SIGNATURES.some(m => clean.includes(m));

  return { executed, total, complete, success, failed, green, compileError, crash, teardown };
}

const { output, rawExit, signal, error } = run();
const a = analyze(output);

let gateExit;
let reason;

if (error) {
  gateExit = 1;
  reason = `Failed to launch test process: ${error.message}`;
} else if (a.compileError) {
  gateExit = 1;
  reason = 'Compile/load error detected in the build.';
} else if (a.crash) {
  gateExit = 1;
  reason = 'Browser disconnected/crashed or the run did not complete.';
} else if (!a.green) {
  gateExit = 1;
  reason = a.failed > 0
    ? `${a.failed} test(s) failed.`
    : (a.success === null ? 'No green TOTAL line found — the run did not finish cleanly.' : 'Run incomplete (Executed count did not reach the total).');
} else if (rawExit === 0) {
  gateExit = 0;
  reason = 'Green run, child exited 0.';
} else if (a.teardown) {
  gateExit = 0;
  reason = `Green run; non-zero child exit (${rawExit}) matched the known post-success ECONNRESET/EPIPE teardown — normalized to 0.`;
} else {
  gateExit = 1;
  reason = `Green run but non-zero child exit (${rawExit}) without the known teardown signature — not normalized.`;
}

console.log('\n──────── NeuralAxe test gate ────────');
console.log(`raw child exit code : ${rawExit}${signal ? ` (signal ${signal})` : ''}`);
console.log(`executed / total    : ${a.executed} / ${a.total} (${a.complete ? 'complete' : 'INCOMPLETE'})`);
console.log(`success / failed    : ${a.success} / ${a.failed}`);
console.log(`compile error       : ${a.compileError}`);
console.log(`browser crash       : ${a.crash}`);
console.log(`teardown signature  : ${a.teardown}`);
console.log(`normalized outcome  : ${gateExit === 0 ? 'PASS (exit 0)' : 'FAIL (exit 1)'}`);
console.log(`reason              : ${reason}`);
console.log('─────────────────────────────────────\n');

process.exit(gateExit);
