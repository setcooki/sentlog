// @sentlog/react-native — TypeScript surface entry point.
//
// Phase 6a: types are re-exported from the canonical
// shared/types/sentlog.ts (synced into src/types.ts via
// tools/sync-types.sh).
// Phase 6b: Sentlog object (NativeModules.Sentlog wrapper) +
// onTelemetry helper. Native module classes land in 6c (Android)
// and 6d (iOS).

export * from "./types";
export { Sentlog, onTelemetry } from "./sentlog";

export const SENTLOG_RN_VERSION = "0.0.0-phase6b";
