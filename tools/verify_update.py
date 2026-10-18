#!/usr/bin/env python3
"""Check a Quartix.exe.sig against a public key and the executable, the same way
Quartix.exe does (minus the version check). Used by the release workflow so a
signing secret that does not match keys/update_pubkey.txt fails the release.

    verify_update.py <update_pubkey.txt> <Quartix.exe> <Quartix.exe.sig>
"""
import hashlib
import sys

from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey


def main(pub_path, exe, sig_path):
    pub = Ed25519PublicKey.from_public_bytes(bytes.fromhex(open(pub_path).read().strip()))
    text = open(sig_path, "rb").read().decode("ascii")
    body, _, sig_line = text.rpartition("sig=")
    lines = body.split("\n")
    if not body.startswith("quartix-update-v1\n") or len(lines) != 4 or not lines[2].startswith("sha256="):
        sys.exit("malformed signature file")
    pub.verify(bytes.fromhex(sig_line.strip()), body.encode("ascii"))  # raises if invalid
    if lines[2][7:] != hashlib.sha256(open(exe, "rb").read()).hexdigest():
        sys.exit("signature does not match the executable")
    print("signature OK for", lines[1])


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
