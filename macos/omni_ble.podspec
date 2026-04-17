#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint omni_ble.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'omni_ble'
  s.version          = '0.1.0-dev.1'
  s.summary          = 'Flutter Bluetooth LE plugin for central and peripheral roles.'
  s.description      = <<-DESC
Flutter Bluetooth Low Energy plugin with a Dart-first API for central and
peripheral roles across iOS, macOS, Android, Windows, and Linux.
                       DESC
  s.homepage         = 'https://github.com/Atrac613/omni_ble'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'Osamu Noguchi' => 'atrac613@gmail.com' }

  s.source           = { :path => '.' }
  s.source_files = 'Classes/**/*'

  # If your plugin requires a privacy manifest, for example if it collects user
  # data, update the PrivacyInfo.xcprivacy file to describe your plugin's
  # privacy impact, and then uncomment this line. For more information,
  # see https://developer.apple.com/documentation/bundleresources/privacy_manifest_files
  # s.resource_bundles = {'omni_ble_privacy' => ['Resources/PrivacyInfo.xcprivacy']}

  s.dependency 'FlutterMacOS'

  s.platform = :osx, '10.11'
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }
  s.swift_version = '5.0'
end
