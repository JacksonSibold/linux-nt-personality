#!/bin/sh
echo "hello"
echo "hello" > /mnt/share/.dodresult/hello.log
echo 42 > /mnt/share/.dodresult/result.exitcode
exit 42
