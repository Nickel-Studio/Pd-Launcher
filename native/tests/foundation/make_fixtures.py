import base64
import json
import sys
from pathlib import Path

from cryptography.hazmat.primitives.ciphers.aead import AESGCM


def envelope(key, nonce, plaintext):
    payload = nonce + AESGCM(key).encrypt(nonce, plaintext, None)
    return "v1:" + base64.b64encode(payload).decode("ascii")


def main():
    output = Path(sys.argv[1])
    key = bytes(range(32))
    nonce = bytes(range(12))
    cases = []
    for index, text in enumerate(["", "synthetic-password", "p@ss word \u5bc6\u7801", "\u96ea/\U0001f680/quote\"\\\n"]):
        current_nonce = nonce[:-1] + bytes([nonce[-1] + index])
        cases.append({"key": key.hex(), "nonce": current_nonce.hex(), "plaintext": text,
                      "sealed": envelope(key, current_nonce, text.encode("utf-8"))})
    bad_utf8 = envelope(key, nonce, bytes([0xff, 0xfe]))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({"cases": cases, "invalidUtf8": bad_utf8}, ensure_ascii=True, indent=2), encoding="ascii")
    print(f"Independent Python cryptography AES256GCM fixtures: {len(cases)} + invalid UTF-8 -> {output}")


if __name__ == "__main__":
    main()
