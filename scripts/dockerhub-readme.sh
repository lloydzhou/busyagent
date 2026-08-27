#!/bin/sh
# Sync docker/README.md to Docker Hub as the repository full description.
# Usage:
#   DOCKERHUB_USERNAME=lloydzhou DOCKERHUB_TOKEN=<PAT> \
#     sh scripts/dockerhub-readme.sh [repo ...]
#   repos default to: lloydzhou/busyagent lloydzhou/busybox
set -e
USER="${DOCKERHUB_USERNAME:?export DOCKERHUB_USERNAME first}"
TOKEN="${DOCKERHUB_TOKEN:?export DOCKERHUB_TOKEN (PAT with read/write) first}"
README_FILE="$(dirname "$0")/../docker/README.md"
REPOS="${*:-lloydzhou/busyagent lloydzhou/busybox}"

# 1) JWT for the hub API
JWT=$(curl -sS -X POST https://hub.docker.com/v2/users/login/ \
    -H 'Content-Type: application/json' \
    -d "{\"username\":\"$USER\",\"password\":\"$TOKEN\"}" |
    sed -n 's/.*"token":"\([^"]*\)".*/\1/p')
[ -n "$JWT" ] || { echo "login failed" >&2; exit 1; }

# 2) patch full_description for each repo
DESC=$(python3 -c 'import json,sys; print(json.dumps({"full_description": open(sys.argv[1]).read()}))' "$README_FILE")

for repo in $REPOS; do
    code=$(curl -sS -o /dev/null -w '%{http_code}' -X PATCH \
        "https://hub.docker.com/v2/repositories/$repo/" \
        -H "Authorization: JWT $JWT" \
        -H 'Content-Type: application/json' \
        -d "$DESC")
    echo "$repo -> HTTP $code"
    [ "$code" = 200 ] || exit 1
done
echo "readme synced"
