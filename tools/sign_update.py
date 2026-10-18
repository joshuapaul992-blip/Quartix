#!/usr/bin/env python3
"""Create the Quartix update signing key and sign release builds.

    sign_update.py keygen <private-key-file>
        Writes a new Ed25519 private key (keep it secret, never commit it) and
        prints the public key to put in keys/update_pubkey.txt.

    sign_update.py sign <private-key-file> <Quartix.exe> <version>
        Writes <Quartix.exe>.sig for `Quartix.exe apply-update`.

Requires the `cryptography` package.
"""
import hashlib
import os
import sys

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey


def keygen(path):
    key = Ed25519PrivateKey.generate()
    pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                            serialization.NoEncryption())
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(pem)
    pub = key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    print(pub.hex())


def sign(key_path, exe, version):
    parts = version.split(".")
    if len(parts) != 3 or not all(p.isdigit() and int(p) <= 0xFFFF for p in parts):
        sys.exit("version must be major.minor.patch")
    key = serialization.load_pem_private_key(open(key_path, "rb").read(), None)
    digest = hashlib.sha256(open(exe, "rb").read()).hexdigest()
    body = "quartix-update-v1\nversion=%s\nsha256=%s\n" % (version, digest)
    sig = key.sign(body.encode("ascii")).hex()
    with open(exe + ".sig", "w", newline="\n") as f:
        f.write(body + "sig=" + sig + "\n")
    print(exe + ".sig")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "keygen":
        keygen(sys.argv[2])
    elif len(sys.argv) == 5 and sys.argv[1] == "sign":
        sign(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        sys.exit(__doc__)
