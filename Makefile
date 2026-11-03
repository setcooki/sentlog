# Sentlog — top-level targets
#
# CI is defined in .github/workflows/ci.yml.

.PHONY: help setup build build-shared build-android build-ios build-rn build-capacitor \
        test test-shared test-android test-ios test-bridges \
        benchmark verify lint clean

help:
	@echo "Sentlog — common targets"
	@echo ""
	@echo "  make setup            list required toolchains"
	@echo "  make build            build everything"
	@echo "  make build-shared     build the C++ core (shared/)"
	@echo "  make build-android    build the AAR (android/)"
	@echo "  make build-ios        build the Swift package"
	@echo "  make build-rn         build @sentlog/react-native (rn/)"
	@echo "  make build-capacitor  build @sentlog/capacitor (capacitor/)"
	@echo "  make test             run C++ core and bridge tests"
	@echo "  make test-android     run Android unit tests"
	@echo "  make test-ios         run iOS tests"
	@echo "  make benchmark        run Android benchmarks"
	@echo "  make verify           run tools/evidence-verifier selftest"
	@echo "  make lint             typecheck bridges, check types parity"
	@echo "  make clean            remove build outputs"

setup:
	@echo "[setup] install CMake, Gradle, Android SDK + NDK, Xcode CLI, Node, Python"

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
	@swift build

test-ios:
	@swift test

build-rn:
	@cd rn && npm ci --no-audit --no-fund --silent && npm run build --silent

build-capacitor:
	@cd capacitor && npm ci --no-audit --no-fund --silent && npm run build --silent

test: test-shared test-bridges

test-shared: build-shared
	@cd shared/build && ctest --output-on-failure

test-bridges: build-rn build-capacitor
	@cd rn && npm test --silent
	@cd capacitor && npm test --silent
	@bash tools/check-types-parity.sh

benchmark:
	@cd android && gradle :benchmarks:connectedReleaseAndroidTest \
		-Pandroid.testInstrumentationRunnerArguments.androidx.benchmark.suppressErrors=DEBUGGABLE,EMULATOR
	@echo "[benchmark] done. Run tools/benchmark/check-thresholds.py on the JSON output."

verify:
	@python3 tools/evidence-verifier/selftest.py

lint: build-rn build-capacitor
	@cd rn && npm run lint --silent
	@cd capacitor && npm run lint --silent
	@bash tools/check-types-parity.sh

clean:
	rm -rf shared/build
	rm -rf android/sentlog/build android/benchmarks/build
	rm -rf .build .swiftpm
	rm -rf rn/node_modules rn/dist
	rm -rf capacitor/node_modules capacitor/dist
