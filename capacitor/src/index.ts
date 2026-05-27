// @sentlog/capacitor — TypeScript surface entry point.
//
// Phase 6a: types re-exported from the canonical
// shared/types/sentlog.ts (synced into src/types.ts via
// tools/sync-types.sh).
// Phase 6e: Sentlog object (registerPlugin wrapper) + onTelemetry
// helper. Native plugin classes land in 6f (Android) and 6g (iOS).

export * from "./types";
export { Sentlog, onTelemetry } from "./sentlog";

export const SENTLOG_CAPACITOR_VERSION = "0.0.0-phase6e";
