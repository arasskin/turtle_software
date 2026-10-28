Pod::Spec.new do |s|
  s.name = 'Datafriend'
  s.version = '0.1.0'
  s.summary = 'Independent native persistence and synchronous query views'
  s.homepage = 'https://github.com/arasskin/DrawDo'
  s.author = 'DrawDo contributors'
  s.license = { :type => 'EPL-1.0', :file => 'NOTICE.md' }
  s.source = { :git => 'https://github.com/arasskin/DrawDo.git' }
  s.platform = :ios, '13.0'
  s.source_files = '*.{c,h}'
  s.exclude_files = 'keystore.c'
  s.header_mappings_dir = '.'
  s.private_header_files = '{common,store,flush}.h'
  s.public_header_files = '{db,schema,c_api,core,engine,network,identity}.h'
  s.frameworks = 'Security', 'CoreFoundation'
  s.pod_target_xcconfig = {
    'GCC_C_LANGUAGE_STANDARD' => 'c99',
    'GCC_PREPROCESSOR_DEFINITIONS' => '$(inherited) MDB_HASH_SIZE=8 MDB_USE_POSIX_SEM=1',
    'HEADER_SEARCH_PATHS' => '$(inherited) "${PODS_TARGET_SRCROOT}/.." "${PODS_TARGET_SRCROOT}" "${PODS_TARGET_SRCROOT}/../third_party/monocypher" "${PODS_TARGET_SRCROOT}/../third_party/xxhash" "${PODS_TARGET_SRCROOT}/../third_party/aelmdb/src" "${PODS_TARGET_SRCROOT}/../littlebear"',
    'DEAD_CODE_STRIPPING' => 'NO'
  }
end
