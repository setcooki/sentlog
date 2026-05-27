// @sentlog/react-native — TypeScript surface entry point.
//
// Phase 6a: types are re-exported from the canonical
// shared/types/sentlog.ts (synced into src/types.ts via
// tools/sync-types.sh). The actual NativeModule shim lands in
// chunks 6b/6c/6d alongside the per-platform Kotlin + Swift
// bridges.

export * from "./types";

export const SENTLOG_RN_VERSION = "0.0.0-phase6a";
