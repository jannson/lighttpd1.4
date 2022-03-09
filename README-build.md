
list(APPEND CMAKE_PREFIX_PATH "/projects/source-build/x86/usr")

cmake -DWITH_WEBDAV_PROPS=0 -DWITH_WEBDAV_LOCKS=1 ..

cat ../lighttpd.conf | ./build/lighttpd -f - -D


curl -H "Accept: application/json" -H "Content-type: application/json" -X POST -d '{"from":1, "size":11}'  http://localhost:3030/api/ddnsto/wait/
