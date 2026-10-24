#!/usr/bin/env python3
"""The suite's licence format from outside the plugins: the reference signer and the test vectors.

A licence is one line, RTL1.<payload>.<signature> (docs/licence/licence-format.md): the payload a compact JSON
object in base64url without padding, the signature Ed25519 (RFC 8032) over the ASCII bytes "RTL1." + the payload's
text, in base64url without padding. The plugins check it (licence/Licence.h) against the public keys they
carry; the licensing service makes it. This script is a third implementation, on the `cryptography` package (OpenSSL's
Ed25519), so the C++ checker, the service and this agree on bytes made by an implementation neither of them is.

    python3 tools/licence.py vectors                 write licence/tests/vectors.json
    python3 tools/licence.py vectors --check         the committed vectors are what this writes
    python3 tools/licence.py keygen <seed-file>      a new signing key: its 32-byte seed in hex to <seed-file> (0600),
                                                     its public half printed as the C++ PublicKeys.h wants it
    python3 tools/licence.py sign <seed-file> --kid ID --name NAME --email EMAIL [--major N] [--issued YYYY-MM-DD]
                                  [--serial RT-...] [--serial-secret-file F --counter N] [--source manual]
                                                     a licence, printed

Run it with a Python that has `cryptography` (pip install cryptography; the repository's .venv). A seed file is a
private key: it never goes into a repository, and the service's lives in its own secret store.
"""

import argparse
import base64
import datetime
import hashlib
import hmac
import json
import os
import sys
from pathlib import Path

try:
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
except ImportError:
    sys.exit("tools/licence.py needs the cryptography package: pip install cryptography")

ROOT = Path(__file__).resolve().parent.parent
VECTORS = ROOT / "licence" / "tests" / "vectors.json"
PREFIX = "RTL1."
# The payload's members in the order the issuer writes them; a checker reads them in any order.
ORDER = ("v", "kid", "serial", "name", "email", "product", "major", "issued", "source")
CROCKFORD = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"


def b64url(data):
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def payload_text(payload):
    """The payload as the issuer writes it: the known members in ORDER, any others after them in the order given,
    compact, UTF-8 as itself."""
    ordered = {key: payload[key] for key in ORDER if key in payload}
    ordered.update({key: value for key, value in payload.items() if key not in ordered})
    return json.dumps(ordered, ensure_ascii=False, separators=(",", ":"))


def make_licence(seed, payload):
    key = Ed25519PrivateKey.from_private_bytes(seed)
    signed = PREFIX + b64url(payload_text(payload).encode("utf-8"))
    return signed + "." + b64url(key.sign(signed.encode("ascii")))


def public_of(seed):
    return Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(Encoding.Raw, PublicFormat.Raw)


def normalised_email(email):
    """An address as a serial is made from it: trimmed, its ASCII letters in lower case (nothing else is folded, so
    every implementation agrees whatever its Unicode tables)."""
    return "".join(c.lower() if "A" <= c <= "Z" else c for c in email.strip())


def serial_of(secret, email, counter):
    """RT- and twelve Crockford base-32 characters in threes of four: the first 60 bits of HMAC-SHA256 under the
    service's serial secret of the normalised address, a line feed and the counter in decimal - the same address gives
    a new serial by its counter, and a serial says nothing of the address without the secret."""
    digest = hmac.new(secret, f"{normalised_email(email)}\n{counter}".encode("utf-8"), hashlib.sha256).digest()
    bits = int.from_bytes(digest[:8], "big") >> 4
    chars = "".join(CROCKFORD[(bits >> (5 * (11 - i))) & 31] for i in range(12))
    return f"RT-{chars[0:4]}-{chars[4:8]}-{chars[8:12]}"


def test_seed(label):
    return hashlib.sha256(f"reckless technology licence test key: {label}".encode("ascii")).digest()


def vectors():
    """Every case the checker is held to, made from two fixed test keys - never a key any build accepts."""
    seed1, seed2 = test_seed("test-1"), test_seed("test-2")
    serial_secret = hashlib.sha256(b"reckless technology licence test serial secret").digest()
    base = {"v": 1, "kid": "test-1", "serial": serial_of(serial_secret, "Jo.Buyer@Example.com", 1),
            "name": "Jo Buyer", "email": "jo.buyer@example.com", "product": "suite", "major": 1,
            "issued": "2026-11-02", "source": "store"}

    def variant(**changes):
        payload = dict(base)
        payload.update(changes)
        return payload

    good = make_licence(seed1, base)
    head, signature = good.rsplit(".", 1)
    other_payload = b64url(payload_text(variant(email="someone.else@example.com")).encode("utf-8"))

    cases = [
        ("valid", good, "valid"),
        ("valid, a name and an address outside ASCII",
         make_licence(seed1, variant(name="Zoë Ångström-Łukasiewicz", email="zoë+plugins@exämple.de")), "valid"),
        ("valid, members the checker does not know", make_licence(seed1, variant(note="press copy", seats=3)),
         "valid"),
        ("valid, a later major version", make_licence(seed1, variant(major=7)), "valid"),
        ("valid, wrapped at 40 characters as an email wraps it",
         "\r\n".join(good[i:i + 40] for i in range(0, len(good), 40)), "valid"),
        ("valid, with spaces and tabs round it", "  \t" + good + " \n", "valid"),
        ("revoked serial", make_licence(seed1, variant(serial="RT-0000-0000-REVK")), "revoked"),
        ("another product", make_licence(seed1, variant(product="amp")), "wrong-product"),
        ("an earlier major version", make_licence(seed1, variant(major=0)), "older-major-version"),
        ("a key the build does not hold", make_licence(seed2, variant(kid="test-2")), "unknown-key"),
        ("signed by another key than its kid names", make_licence(seed2, base), "bad-signature"),
        ("its payload swapped for another under the same signature", PREFIX + other_payload + "." + signature,
         "bad-signature"),
        ("its signature's first byte altered",
         head + "." + b64url(bytes([base64.urlsafe_b64decode(signature + "==")[0] ^ 1]) +
                             base64.urlsafe_b64decode(signature + "==")[1:]), "bad-signature"),
        ("no RTL1 prefix", good[len(PREFIX):], "malformed"),
        ("RTL2", "RTL2." + good[len(PREFIX):], "malformed"),
        ("padded base64", head + "." + signature + "==", "malformed"),
        ("standard base64's alphabet", head.replace("-", "+").replace("_", "/") + "." + signature
         if ("-" in head or "_" in head) else head + "+." + signature, "malformed"),
        ("a third dot", good + ".AAAA", "malformed"),
        ("a short signature", head + "." + signature[:40], "malformed"),
        ("version 2 of the payload", make_licence(seed1, variant(v=2)), "malformed"),
        ("a fractional major version", make_licence(seed1, variant(major=1.5)), "malformed"),
        ("no email", make_licence(seed1, {k: v for k, v in base.items() if k != "email"}), "malformed"),
        ("an empty name", make_licence(seed1, variant(name="")), "malformed"),
        ("a payload that is not JSON", PREFIX + b64url(b"not json") + "." + signature, "malformed"),
        ("empty", "", "malformed"),
    ]
    return {
        "comment": "Written by tools/licence.py vectors from two fixed test keys that no build accepts. "
                   "Every implementation of the licence format is held to these bytes: docs/licence/"
                   "licence-format.md.",
        "format": "RTL1",
        "keys": [{"kid": "test-1", "seed": seed1.hex(), "public": public_of(seed1).hex()},
                 {"kid": "test-2", "seed": seed2.hex(), "public": public_of(seed2).hex()}],
        "rules": {"keys": ["test-1"], "revoked": ["RT-0000-0000-REVK"], "suite_major": 1, "product": "suite"},
        "signing": [{"kid": "test-1", "payload": payload_text(p), "licence": make_licence(seed1, p)}
                    for p in (base, variant(name="Zoë Ångström-Łukasiewicz", email="zoë+plugins@exämple.de"))],
        "serials": {"secret": serial_secret.hex(),
                    "cases": [{"email": e, "counter": c, "serial": serial_of(serial_secret, e, c)}
                              for e, c in (("Jo.Buyer@Example.com", 1), ("  jo.buyer@example.com ", 1),
                                           ("jo.buyer@example.com", 2), ("ZOË@Exämple.DE", 1))]},
        "licences": [{"case": name, "text": text, "verdict": verdict} for name, text, verdict in cases],
    }


def write_vectors(check):
    text = json.dumps(vectors(), ensure_ascii=False, indent=2) + "\n"
    if check:
        current = VECTORS.read_text(encoding="utf-8").replace("\r\n", "\n") if VECTORS.exists() else ""
        if current != text:
            sys.exit(f"{VECTORS.relative_to(ROOT)} is not what tools/licence.py writes: run it again")
        print(f"{VECTORS.relative_to(ROOT)} is current")
        return
    VECTORS.parent.mkdir(parents=True, exist_ok=True)
    VECTORS.write_bytes(text.encode("utf-8"))
    print(f"wrote {VECTORS.relative_to(ROOT)}")


def read_seed(path):
    seed = bytes.fromhex(Path(path).read_text().strip())
    if len(seed) != 32:
        sys.exit(f"{path} does not hold a 32-byte seed in hex")
    return seed


def keygen(path):
    out = Path(path)
    if out.exists():
        sys.exit(f"{out} exists: a key is never written over")
    seed = os.urandom(32)
    descriptor = os.open(out, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, "w") as f:
        f.write(seed.hex() + "\n")
    public = public_of(seed)
    print(f"seed written to {out} (keep it secret; it is the key)")
    print(f"public key: {public.hex()}")
    print("PublicKeys.h: PublicKey{\"<kid>\", {" + ", ".join(f"0x{b:02x}" for b in public) + "}},")


def sign(args):
    seed = read_seed(args.seed_file)
    serial = args.serial
    if serial is None:
        if args.serial_secret_file is None:
            sys.exit("give --serial, or --serial-secret-file and --counter")
        serial = serial_of(read_seed(args.serial_secret_file), args.email, args.counter)
    payload = {"v": 1, "kid": args.kid, "serial": serial, "name": args.name, "email": args.email.strip(),
               "product": "suite", "major": args.major, "issued": args.issued, "source": args.source}
    print(make_licence(seed, payload))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    vec = commands.add_parser("vectors")
    vec.add_argument("--check", action="store_true")
    gen = commands.add_parser("keygen")
    gen.add_argument("seed_file")
    sig = commands.add_parser("sign")
    sig.add_argument("seed_file")
    sig.add_argument("--kid", required=True)
    sig.add_argument("--name", required=True)
    sig.add_argument("--email", required=True)
    sig.add_argument("--major", type=int, default=1)
    sig.add_argument("--issued", default=datetime.date.today().isoformat())
    sig.add_argument("--serial")
    sig.add_argument("--serial-secret-file")
    sig.add_argument("--counter", type=int, default=1)
    sig.add_argument("--source", default="manual")
    args = parser.parse_args()
    if args.command == "vectors":
        write_vectors(args.check)
    elif args.command == "keygen":
        keygen(args.seed_file)
    else:
        sign(args)


if __name__ == "__main__":
    main()
