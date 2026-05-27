# @sentlog/capacitor — iOS podspec.
#
# Consumer apps' Capacitor `pod install` picks this up via the
# Capacitor plugin discovery (auto-generated Podfile entry from the
# npm package's `capacitor.ios.src` setting).

require 'json'
package = JSON.parse(File.read(File.join(__dir__, '..', 'package.json')))

Pod::Spec.new do |s|
  s.name             = "SentlogCapacitor"
  s.version          = package['version']
  s.summary          = package['description']
  s.license          = package['license']
  s.homepage         = "https://github.com/setcooki/sentlog"
  s.authors          = { "Sentlog" => "set@cooki.me" }
  s.platforms        = { :ios => "16.0" }
  s.source           = { :git => "https://github.com/setcooki/sentlog.git" }
  s.source_files     = "Plugin/**/*.{swift,h,m}"
  s.requires_arc     = true
  s.swift_version    = "5.9"

  s.dependency "Capacitor"
  s.dependency "Sentlog"
end
