
list(APPEND CMAKE_PREFIX_PATH "/projects/source-build/x86/usr")

cmake -DWITH_WEBDAV_PROPS=0 -DWITH_WEBDAV_LOCKS=1 ..

