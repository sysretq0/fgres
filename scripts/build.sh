#!/bin/sh
# fgres build: everything lives in the Makefile (tests + host + android).
set -e
cd "$(dirname "$0")/.."
exec make all
