// Minimal @capacitor/core type stubs for the @sentlog/capacitor
// build. The full @capacitor/core install is fine but unnecessary
// in CI just to typecheck our plugin wrapper. At consumer time
// the real @capacitor/core shadows these stubs.
//
// If a future plugin method needs a symbol that's not declared
// here, add it.

declare module "@capacitor/core" {
    export interface PluginListenerHandle {
        remove(): Promise<void>;
    }

    export function registerPlugin<T>(name: string): T;
}
