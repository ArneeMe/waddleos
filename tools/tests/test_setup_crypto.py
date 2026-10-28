"""The setup tool's own X25519 and ChaCha20-Poly1305, against the RFCs and the
firmware (protocol/vectors.json, written by test/mesh/run.sh)."""

import importlib.machinery
import importlib.util
import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
loader = importlib.machinery.SourceFileLoader("waddle_mesh", str(ROOT / "tools" / "waddle-mesh"))
spec = importlib.util.spec_from_loader("waddle_mesh", loader)
wm = importlib.util.module_from_spec(spec)
loader.exec_module(wm)
V = json.loads((ROOT / "protocol" / "vectors.json").read_text())


class SetupCrypto(unittest.TestCase):
    def test_x25519_rfc7748(self):
        k = bytes.fromhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4")
        u = bytes.fromhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")
        self.assertEqual(wm.x25519(k, u).hex(), "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552")
        alice = bytes.fromhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
        self.assertEqual(wm.x25519_public(alice).hex(),
                         "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a")

    def test_aead_rfc8439(self):
        key = bytes(range(0x80, 0xA0))
        nonce = bytes.fromhex("070000004041424344454647")
        aad = bytes.fromhex("50515253c0c1c2c3c4c5c6c7")
        plain = (b"Ladies and Gentlemen of the class of '99: If I could offer you only one tip for "
                 b"the future, sunscreen would be it.")
        sealed = wm.aead_seal(key, nonce, plain, aad)
        self.assertEqual(sealed[-16:].hex(), "1ae10b594f09e26a7e902ecbd0600691")
        self.assertEqual(sealed[:16].hex(), "d31a8d34648e60db7b86afbc53ef7ec2")
        self.assertEqual(wm.aead_open(key, nonce, sealed, aad), plain)
        with self.assertRaises(RuntimeError):
            wm.aead_open(key, nonce, sealed[:-1] + b"\x00", aad)

    def test_same_as_the_firmware(self):
        s = V["setup"]
        tool_secret = bytes.fromhex(s["toolSecret"])
        unit_public = bytes.fromhex(V["memberA"]["public"])
        key = wm.setup_key(tool_secret, wm.x25519_public(tool_secret), unit_public)
        self.assertEqual(key.hex(), s["key"])
        sender = wm.aead_open(key, wm.setup_nonce(1), bytes.fromhex(s["senderKeyBox"]))
        self.assertEqual(sender.hex(), V["senderKey"])
        self.assertEqual(wm.setup_code(unit_public), s["code"])


if __name__ == "__main__":
    unittest.main()
