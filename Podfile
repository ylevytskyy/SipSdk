platform :ios, '12.1'

target 'TareSIPDemo' do
  use_frameworks!

  # Keep the legacy native SIP engine explicit until a replacement is validated.
  pod 'taresip', '= 0.1.1'
end

# The legacy pod declares an iOS 7 minimum. Match it to the app's deployment
# target to avoid building the pod with a deployment target unsupported by
# current Xcode versions.
post_install do |installer|
  minimum_deployment_target = Gem::Version.new('12.1')

  installer.pods_project.targets.each do |target|
    target.build_configurations.each do |config|
      deployment_target = config.build_settings['IPHONEOS_DEPLOYMENT_TARGET']
      next unless deployment_target

      if Gem::Version.new(deployment_target) < minimum_deployment_target
        config.build_settings['IPHONEOS_DEPLOYMENT_TARGET'] = minimum_deployment_target.to_s
      end
    end
  end
end
