#!/usr/bin/env python3
"""Gzip a file reproducibly (fixed mtime, no file name) for embedding in firmware."""
import gzip
import sys

src, dst = sys.argv[1], sys.argv[2]
with open(src, 'rb') as f:
    data = f.read()
with open(dst, 'wb') as f:
    f.write(gzip.compress(data, compresslevel=9, mtime=0))
