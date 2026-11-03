# @sentlog/react-native — iOS podspec.
#
# Consumer apps' React Native autolinking discovers this podspec
# via the npm package and pulls it into their Podfile during
# `pod install`. It must stay in the package root, the only place
# autolinking looks. The Sentlog Swift library comes from the Swift package
# at the repo root, added through React Native's spm_dependency helper
# (React Native 0.75+). There is no Sentlog pod.

Pod::Spec.new do |s|
  s.name             = "SentlogRn"
  s.version          = "0.0.0"
  s.summary          = "React Native bridge for Sentlog — universal device-first evidence and presence SDK"
  s.license          = "MIT"
  s.homepage         = "https://github.com/setcooki/sentlog"
  s.authors          = { "Sentlog" => "set@cooki.me" }
  s.platforms        = { :ios => "16.0" }
  s.source           = { :git => "https://github.com/setcooki/sentlog.git" }
  s.source_files     = "ios/*.{h,m,mm,swift}"
  s.requires_arc     = true
  s.swift_version    = "5.9"

  s.dependency "React-Core"
  # A static pod doesn't pass the Swift package's ONNX Runtime binary on
  # to the app, so link the same library from its pod. Keep this version
  # in step with the root Package.swift.
  s.dependency "onnxruntime-c", "1.24.2"
  # Replace the branch pin with a version once releases are tagged.
  spm_dependency(s,
    url: "https://github.com/setcooki/sentlog.git",
    requirement: { kind: "branch", branch: "main" },
    products: ["Sentlog"])
end
