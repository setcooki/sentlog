// Minimal React Native type stubs for the @sentlog/react-native
// build. The full `react-native` package is a >900 MB install when
// node_modules pulls in its native sources; CI doesn't need any of
// that to typecheck our wrapper. At consumer time, RN provides the
// real types via its own package.json `types` entry, which shadows
// these stubs.
//
// If a future bridge method needs an RN symbol that's not declared
// here, add it.

declare module "react-native" {
    export interface NativeEventSubscription {
        remove(): void;
    }

    export class NativeEventEmitter {
        constructor(nativeModule?: object);
        addListener(eventType: string,
                    listener: (event: any) => void): NativeEventSubscription;
        removeAllListeners(eventType: string): void;
    }

    export const NativeModules: Record<string, unknown>;
}
