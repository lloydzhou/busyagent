#!/bin/sh
for i in 1 2 3; do
  if nc -z -w2 host.docker.internal 8317; then echo "conn $i OK"; else echo "conn $i FAIL"; fi
done
echo "--- /etc/hosts ---"
cat /etc/hosts
