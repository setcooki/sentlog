// Obj-C bridge for the Swift @objc(SentlogModule) class. React
// Native's RCT_EXTERN_MODULE wires the JS-exposed name "Sentlog"
// to the Swift implementation in SentlogModule.swift; each
// RCT_EXTERN_METHOD declaration exposes one method to the JS side.

#import <React/RCTBridgeModule.h>
#import <React/RCTEventEmitter.h>

@interface RCT_EXTERN_REMAP_MODULE(Sentlog, SentlogModule, RCTEventEmitter)

RCT_EXTERN_METHOD(version
                  : (RCTPromiseResolveBlock) resolve
                  rejecter : (RCTPromiseRejectBlock) reject)

RCT_EXTERN_METHOD(initialize
                  : (NSString *) configJson
                  resolver : (RCTPromiseResolveBlock) resolve
                  rejecter : (RCTPromiseRejectBlock) reject)

RCT_EXTERN_METHOD(startCapture
                  : (RCTPromiseResolveBlock) resolve
                  rejecter : (RCTPromiseRejectBlock) reject)

RCT_EXTERN_METHOD(evaluate
                  : (RCTPromiseResolveBlock) resolve
                  rejecter : (RCTPromiseRejectBlock) reject)

RCT_EXTERN_METHOD(cancel
                  : (RCTPromiseResolveBlock) resolve
                  rejecter : (RCTPromiseRejectBlock) reject)

RCT_EXTERN_METHOD(destroy
                  : (RCTPromiseResolveBlock) resolve
                  rejecter : (RCTPromiseRejectBlock) reject)

RCT_EXTERN_METHOD(getPermissions
                  : (RCTPromiseResolveBlock) resolve
                  rejecter : (RCTPromiseRejectBlock) reject)

RCT_EXTERN_METHOD(getCapabilities
                  : (RCTPromiseResolveBlock) resolve
                  rejecter : (RCTPromiseRejectBlock) reject)

RCT_EXTERN_METHOD(modelVersions
                  : (RCTPromiseResolveBlock) resolve
                  rejecter : (RCTPromiseRejectBlock) reject)

RCT_EXTERN_METHOD(checkForUpdates
                  : (RCTPromiseResolveBlock) resolve
                  rejecter : (RCTPromiseRejectBlock) reject)

RCT_EXTERN_METHOD(setSigningKey
                  : (NSString *) secretKeyBase64
                  verificationMethod : (NSString *) verificationMethod
                  resolver : (RCTPromiseResolveBlock) resolve
                  rejecter : (RCTPromiseRejectBlock) reject)

RCT_EXTERN_METHOD(setEnvelopeDispatcherActive
                  : (BOOL) active
                  resolver : (RCTPromiseResolveBlock) resolve
                  rejecter : (RCTPromiseRejectBlock) reject)

RCT_EXTERN_METHOD(drainQueueOnce
                  : (NSString *) path
                  drainRequestId : (NSString *) drainRequestId
                  resolver : (RCTPromiseResolveBlock) resolve
                  rejecter : (RCTPromiseRejectBlock) reject)

RCT_EXTERN_METHOD(drainQueueReply
                  : (NSString *) drainRequestId
                  line : (nonnull NSNumber *) line
                  keep : (BOOL) keep
                  resolver : (RCTPromiseResolveBlock) resolve
                  rejecter : (RCTPromiseRejectBlock) reject)

+ (BOOL)requiresMainQueueSetup {
    return NO;
}

@end
