// @sentlog/capacitor — TypeScript surface entry point.
//
// Phase 6a: types are re-exported from the canonical
// shared/types/sentlog.ts (synced into src/types.ts via
// tools/sync-types.sh). The actual @CapacitorPlugin shim lands in
// chunks 6e/6f/6g alongside the per-platform Kotlin + Swift
// plugin classes.

export * from "./types";

export const SENTLOG_CAPACITOR_VERSION = "0.0.0-phase6a";
