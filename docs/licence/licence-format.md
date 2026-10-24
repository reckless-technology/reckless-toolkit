# The licence format

A licence to the Reckless Technology suite is one line of text. The plugins check it offline against the public
keys they carry (`licence/`), and the licensing service writes it.

Three implementations are held to the same bytes, the test vectors in
[`licence/tests/vectors.json`](../../licence/tests/vectors.json):
- **the plugins' checker**: Monocypher's Ed25519, CTest `licence`;
- **the reference signer and the vectors' writer**: OpenSSL's Ed25519 through Python's `cryptography`,
  [`tools/licence.py`](../../tools/licence.py);
- **the service**: WebCrypto.

## Why a signature

The plugins can only **check** a licence; nothing in a binary can **make** one.

A licence is signed with Ed25519 (RFC 8032). The private key is the service's alone and is in no repository. The
binaries hold only public keys, so a key generator can't be written from anything that ships.

A scheme with a shared secret would not have this property. An HMAC of the email, or a licence that decrypts code, puts
the secret, or a key common to every copy, inside every binary. Anyone who extracts it can then mint licences for any
name.

What the check does not stop is someone patching a binary to skip it. No offline scheme can. The licence carries the
buyer's name and address, and the panel shows them; that is the deterrent against sharing.

## The text

```
RTL1.<payload>.<signature>
```

| Part | What it is |
|---|---|
| `RTL1.` | The format and its version. A later format takes another prefix. |
| `<payload>` | The payload, a compact JSON object, as UTF-8, in base64url (RFC 4648 section 5) with no padding. |
| `<signature>` | The Ed25519 signature, 64 bytes, in base64url with no padding. |

The signature covers the ASCII bytes of `RTL1.` followed by the payload's base64url text, exactly as they appear.
Because the bytes signed are the bytes carried, the payload never has to be put in a canonical form before it is
checked.

### Rules for reading the text

- **White space** (spaces, tabs, line breaks) anywhere in the text is ignored, so a licence wrapped by an email client
  still reads.
- **Base64url is strict.** These are all refused:
  - `+`, `/` or `=`;
  - a length that leaves a lone character;
  - a last character carrying bits past the last byte.

  So each run of bytes has exactly one spelling.
- **Length:** text longer than 8192 characters is refused unread.

## The payload

The service writes the members in the order below, then any later additions. A checker reads them in any order and
passes over a member it doesn't know. The order matters only to an implementation that reproduces a licence byte for
byte.

| Member | Type | Meaning |
|---|---|---|
| `v` | number | 1 |
| `kid` | string | The id of the key that signed it: which public key checks it |
| `serial` | string | `RT-XXXX-XXXX-XXXX`, below |
| `name` | string | The licensee's name, as the panel shows it |
| `email` | string | The licensee's address, trimmed |
| `product` | string | `suite`: the whole suite. Other values are reserved for products sold apart. |
| `major` | number | The newest major version the licence opens: a whole number, 0 to 255 |
| `issued` | string | The date it was issued, `YYYY-MM-DD` |
| `source` | string | Where it came from (`store`, `manual`, `nfr`, `replacement`), for the records. Checkers ignore it. |

Every member up to `issued` must be present and must not be empty.

The JSON is compact: no space after `:` or `,`. Text outside ASCII is written as itself, not escaped.

## What a checker decides

The verdicts below are listed in the order a checker reaches them. **The signature is checked before anything the
payload says is believed**, so every verdict after `bad-signature` is the issuer's own word.

| Verdict | When |
|---|---|
| `malformed` | Not `RTL1.` text; base64url that doesn't decode strictly; a signature that isn't 64 bytes; a payload that isn't a JSON object of the members above |
| `unknown-key` | Its `kid` names no key this build holds |
| `bad-signature` | The signature is not that key's over the signed bytes: the licence was altered, or made up |
| `wrong-product` | `product` is not the build's (`suite`) |
| `revoked` | Its serial is on the build's list of withdrawn serials: refunded, charged back, or replaced by another licence |
| `older-major-version` | `major` is lower than the suite's MAJOR version |
| `valid` | Everything above passes |

**Upgrades:** a licence opens every version whose MAJOR is no higher than its `major`. Minor releases are free; the next
major is a paid upgrade.

The public keys and the withdrawn serials a build accepts are compiled into it, in `licence/PublicKeys.h`.
- **A key is added when it is made**, and stays while any licence it signed is honoured. Signing can then move to a new
  key (rotation) without old licences going dark.
- **The withdrawn serials** come from the service's records. They are serials only: nothing that identifies a person.

## The serial

The format is `RT-` followed by twelve Crockford base-32 characters, in three groups of four. The characters are the
first 60 bits of:

    HMAC-SHA256(serial secret, normalised address + "\n" + counter in decimal)

- **The normalised address** is the address trimmed of white space at both ends, with its ASCII letters lower-cased.
  Nothing else is folded, so every implementation agrees whatever its Unicode tables.
- **The counter** starts at 1 and moves on when one address is given a second licence.
- **The serial secret** is the service's, a second secret beside its signing key.

The serial is for the records and for support. The service can tell which address a serial belongs to without
storing anything, but nobody without the secret can work back from a serial to an address. It plays no part in the
check: the signature is the security.

## Where a machine keeps it

Every plugin on a machine runs under one licence, held in a file, never in a session or a preset (which go to other
people). The store is `licence/LicenceStore.h`, and it reads two files:

1. **The user's:** `reckless-technology/licence.json` in the user's settings folder:
   - macOS: `~/Library/Application Support`
   - Windows: the roaming application data folder
   - Linux: `$XDG_CONFIG_HOME`, or `~/.config` where unset

   It holds `{"licence": "<the line>"}`. It is written only when the licence checks as valid, so a mistyped licence
   never displaces a good one. On macOS and Linux it is readable by its owner alone (0600).
2. **The machine's:** `reckless-technology/licence.json` in the machine's settings folder:
   - macOS: `/Library/Application Support`
   - Windows: `C:\ProgramData`
   - Linux: the first entry of `$XDG_CONFIG_DIRS`, or `/etc/xdg` where unset

   It is read when the user's file holds no valid licence. An administrator puts it there, for every user of the
   machine, or for a host that keeps its plug-ins out of the user's own folder. It may hold the JSON form or the bare
   line.

The store reads both files when it is made, and reads them again whenever either's size or time changes. A licence
installed by one plugin, format or process is therefore the one every instance runs under. The audio thread reads one
atomic flag.

## What a licence opens: demo mode

A build configured with `RECKLESS_DEMO_MODE` on gates every unlicensed plugin (vst-plugins' `common/core/DemoGate.h`, on the switch `licence/DemoMode.h`): eight
seconds of output, then two of silence, in every ten, the cut and the return along 10 ms raised cosines. The gate
counts in samples from the plugin's prepare, so an offline render is gated as playing is. It sits in the one loop
every format's audio passes through (`core::RunChunks`), between the core and the host's buffers, so a plugin and a
whole Pedalboard are each gated once, and a bypassed block, the dry alone, never.

With a valid licence the gate is a wire, bit for bit; a licence installed while a plugin plays opens it at the next
chunk. Each format points its gate at the store every plugin in the process shares (`licence::Shared()`), and reads
its files again from its message-thread timer at most once a second (`licence::PollShared`).

Until the suite is sold the option is off, and every build plays as it always has. The check, the store and the panel's
dialog are in every build either way.

## Where a person meets it

In a demo build (`RECKLESS_DEMO_MODE` on), every plugin's header has a settings gear left of PRESETS, and its menu
ends with **Licence...**. A build without demo mode - the suite as it is given away - offers no such item, so a player
never meets a licence at all. That opens a card over
the window (`window/LicenceDialog.h`) showing who the licence is issued to, its serial and the version it covers.
Without a valid licence, the card says the plugin is not licensed and, where a licence is on file, why it doesn't
check.

There are two ways in:
- **Paste.** The line is pasted into the card's field, which admits only what a licence is made of, so a wrapped one
  from an email lands whole. **Install** or Return installs it.
- **From file.** **From file...** reads the attachment, or a `licence.json` copied from another machine.

A licence that doesn't check is never written. **Remove** takes the user's licence off the machine, and **Buy...**
opens the shop.

In a demo build with no licence, the header shows **DEMO** beside the version, and the Standalone's bar says so too.
`reckless_<x>_render_panel <out.png> demo licence` draws both.

## Making a licence by hand

`tools/licence.py` is the reference signer. It needs the `cryptography` package, which the repository's `.venv` has.

```
python3 tools/licence.py vectors [--check]    # write, or check, the test vectors
python3 tools/licence.py keygen <seed-file>   # a new signing key: its seed (secret) to the file, its public half printed
python3 tools/licence.py sign <seed-file> --kid ID --name NAME --email EMAIL [--major N] [--serial RT-...]
```

The vectors are made from two fixed test keys, `test-1` and `test-2`, which no build accepts. The suite checks that the
production key list holds no `test-` key.
