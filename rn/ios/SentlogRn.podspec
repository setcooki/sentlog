# @sentlog/react-native — iOS podspec.
#
# Consumer apps' React Native autolinking discovers this podspec
# via the npm package and pulls it into their Podfile during
# `pod install`. The Sentlog Swift library itself is a separate
# SPM/CocoaPod that the integrator wires; see README.md for the
# integration steps.

Pod::Spec.new do |s|
  s.name             = "SentlogRn"
  s.version          = "0.0.0"
  s.summary          = "React Native bridge for Sentlog — universal device-first evidence and presence SDK"
  s.license          = "MIT"
  s.homepage         = "https://github.com/setcooki/sentlog"
  s.authors          = { "Sentlog" => "set@cooki.me" }
  s.platforms        = { :ios => "16.0" }
  s.source           = { :git => "https://github.com/setcooki/sentlog.git" }
  s.source_files     = "*.{h,m,mm,swift}"
  s.requires_arc     = true
  s.swift_version    = "5.9"

  s.dependency "React-Core"
  # Sentlog Swift SDK (the bindings under ios/). Consumers wire
  # this via SPM or a vendored framework; the podspec declares
  # the dependency for completeness.
  s.dependency "Sentlog"
end
