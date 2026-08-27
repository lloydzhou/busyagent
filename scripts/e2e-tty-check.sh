#!/bin/sh
echo "=== tty presence in -it container ==="
docker run --rm -it busyagent-scratch sh -c 'tty; ls /dev | head -5'
