# Sentlog — top-level targets
#
# Phase 0 state: most targets are scaffolding stubs. They succeed without
# doing real work yet — that's intentional. Each target gets real implementation
# in the phase that owns it (see docs/rollout-plan.md).

.PHONY: help setup build build-shared build-android build-ios build-rn build-capacitor \
        test test-shared test-android test-ios test-bridges \
        benchmark verify lint clean

help:
	@echo "Sentlog — common targets"
	@echo ""
	@echo "  make setup            install toolchains (placeholder)"
	@echo "  make build            build everything"
	@echo "  make build-shared     build the C++ core (shared/)"
	@echo "  make build-android    build the AAR (android/)"
	@echo "  make build-ios        build the XCFramework (ios/)"
	@echo "  make build-rn         build @sentlog/react-native (rn/)"
	@echo "  make build-capacitor  build @sentlog/capacitor (capacitor/)"
	@echo "  make test             run all tests"
	@echo "  make benchmark        run tools/benchmark-harness"
	@echo "  make verify           run tools/evidence-verifier on a fixture"
	@echo "  make lint             lint everything"
	@echo "  make clean            remove build outputs"

setup:
	@echo "[setup] Phase 0 stub — install CMake, NDK, Xcode CLI, Node, Python as needed"

build: build-shared build-android build-ios build-rn build-capacitor

build-shared:
	@echo "[build-shared] Configuring CMake (sanitisers on)"
	@cmake -S shared -B shared/build \
		-DCMAKE_BUILD_TYPE=Debug \
		-DSENTLOG_ENABLE_ASAN=ON \
		-DSENTLOG_ENABLE_UBSAN=ON > /dev/null
	@cmake --build shared/build

build-android:
	@cd android && gradle :sentlog:assembleDebug --no-daemon

test-android:
	@cd android && gradle :sentlog:testDebugUnitTest --no-daemon

build-ios:
	@echo "[build-ios] Phase 0 stub — XCFramework build wires up in Phase 1"

build-rn:
	@echo "[build-rn] Phase 0 stub — @sentlog/react-native wires up in Phase 6"

build-capacitor:
	@echo "[build-capacitor] Phase 0 stub — @sentlog/capacitor wires up in Phase 6"

test: test-shared

test-shared: build-shared
	@cd shared/build && ctest --output-on-failure

test-ios:
	@echo "[test-ios] Phase 0 stub"

test-bridges:
	@echo "[test-bridges] Phase 0 stub"

benchmark:
	@echo "[benchmark] Phase 0 stub — wires up in Phase 1"

verify:
	@echo "[verify] Phase 0 stub — wires up in Phase 7"

lint:
	@echo "[lint] Phase 0 stub"

clean:
	rm -rf shared/build
	rm -rf android/sentlog/build android/benchmarks/build
	rm -rf ios/.build ios/.swiftpm
	rm -rf rn/node_modules rn/dist
	rm -rf capacitor/node_modules capacitor/dist
