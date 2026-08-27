#!/bin/sh
# sync agentutils into container (bypasses grpcfuse xattr quirk via tarfile)
set -e
tar c --exclude .git --exclude '*.o' -f /tmp/au.tar agentutils
docker cp /tmp/au.tar bb-build:/tmp/au.tar
docker exec bb-build sh -c 'cd /src && rm -rf agentutils && tar xf /tmp/au.tar && make -C /src >/dev/null 2>&1 && echo built'
