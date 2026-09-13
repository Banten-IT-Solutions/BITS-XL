#!/usr/bin/env python3
# Self-check for the Python bitsxl backend: byte-exact crypto against fixed vectors.
import importlib.util
import sys
import hashlib
import hmac
import base64
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
BACKEND = os.path.join(HERE, "bitsxl", "root", "usr", "bin", "bitsxl")

spec = importlib.util.spec_from_loader("bitsxl", loader=None, origin=BACKEND)
# Load as a module without executing __main__ (guarded), exec the source.
src = open(BACKEND).read()
ns = {"__name__": "bitsxl", "__file__": BACKEND}
exec(compile(src, BACKEND, "exec"), ns)


def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


# --- set CFG to known values for deterministic tests ---
for k in ns["DEFAULTS"]:
    ns["CFG"][k] = ns["DEFAULTS"][k]
ns["CFG"]["XDATA_KEY"] = "0123456789abcdef0123456789abcdef"  # 32 bytes -> AES-256
ns["CFG"]["ENCRYPTED_FIELD_KEY"] = "fedcba9876543210"  # 16 bytes -> AES-128
ns["CFG"]["X_API_BASE_SECRET"] = "secret123"
ns["CFG"]["AX_API_SIG_KEY"] = "sigkey4567"

# 1) AES-256-CBC NIST SP 800-38A vector (single block)
key = bytes.fromhex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4")
iv = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
pt = bytes.fromhex("6bc1bee22e409f96e93d7e117393172a")
expect_ct = bytes.fromhex("f58c4c04d6e5f1ba779eabfb5f7bfbd6")
ct = ns["aes_cbc"](True, key, iv, pt)
if ct != expect_ct:
    fail(f"AES-256-CBC encrypt: got {ct.hex()} want {expect_ct.hex()}")
if ns["aes_cbc"](False, key, iv, ct) != pt:
    fail("AES-256-CBC decrypt round-trip")

# 2) AES-128-CBC NIST vector
key128 = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")
expect_ct128 = bytes.fromhex("7649abac8119b246cee98e9b12e9197d")
ct128 = ns["aes_cbc"](True, key128, iv, pt)
if ct128 != expect_ct128:
    fail(f"AES-128-CBC encrypt: got {ct128.hex()} want {expect_ct128.hex()}")

# 3) encrypt_xdata / decrypt_xdata_payload round-trip (independent expected)
ns["CFG"]["XDATA_KEY"] = "0123456789abcdef0123456789abcdef"
xtime = 1234567890
plain = json.dumps({"is_enterprise": False, "lang": "en"})
enc = ns["encrypt_xdata"](plain, xtime)
# independent recompute
xt = str(xtime)
dig = hashlib.sha256(xt.encode()).digest()
ivx = dig.hex().encode()[:16]
from Crypto.Cipher import AES as _AES

padded = plain.encode() + bytes([16 - (len(plain.encode()) % 16)]) * (
    16 - (len(plain.encode()) % 16)
)
exp = base64.urlsafe_b64encode(
    _AES.new("0123456789abcdef0123456789abcdef".encode(), _AES.MODE_CBC, ivx).encrypt(
        padded
    )
).decode()
if enc != exp:
    fail(f"encrypt_xdata mismatch: got {enc} want {exp}")
# decrypt through backend
payload = json.dumps({"xdata": enc, "xtime": xtime})
dec = ns["decrypt_xdata_payload"](payload)
if dec != plain:
    fail(f"decrypt_xdata_payload round-trip: got {dec!r} want {plain!r}")

# 4) make_x_signature (independent HMAC-SHA512)
id_token = "myidtoken"
method = "POST"
path = "api/v8/profile"
sig_time = 1720000000
key = f"{ns['CFG']['X_API_BASE_SECRET']};{id_token};{method};{path};{sig_time}"
msg = f"{id_token};{sig_time};"
exp215 = hmac.new(key.encode(), msg.encode(), hashlib.sha512).hexdigest()
got = ns["make_x_signature"](id_token, method, path, sig_time)
if got != exp215:
    fail(f"make_x_signature mismatch: got {got} want {exp215}")

# 5) make_ax_api_signature (independent HMAC-SHA256 + b64)
ts = "2026-09-13T08:00:00.000+0700"
contact = "6281234567890"
code = "123456"
ctype = "SMS"
pre = f"{ts}password{ctype}{contact}{code}openid"
exp216 = base64.b64encode(
    hmac.new(
        ns["CFG"]["AX_API_SIG_KEY"].encode(), pre.encode(), hashlib.sha256
    ).digest()
).decode()
got216 = ns["make_ax_api_signature"](ts, contact, code, ctype)
if got216 != exp216:
    fail(f"make_ax_api_signature mismatch: got {got216} want {exp216}")

# 6) encrypted_empty_field shape + decrypt
ns["CFG"]["ENCRYPTED_FIELD_KEY"] = "fedcba9876543210"
eef = ns["encrypted_empty_field"]()
if not eef:
    fail("encrypted_empty_field empty")
# last 8 chars are the hex iv, rest is urlsafe b64 of 16-byte ct
ivhex = eef[-16:]
ctb = base64.urlsafe_b64decode(eef[:-16] + "=="[: (4 - (len(eef[:-16]) % 4)) % 4])
# decrypt with iv=ivhex.encode()
pt2 = ns["aes_cbc"](False, "fedcba9876543210", ivhex.encode(), ctb)
# apply pkcs7 unpad manually: expect 16 bytes of 0x10
if pt2 != bytes([16]) * 16:
    fail(f"encrypted_empty_field decrypt: got {pt2.hex()}")

# 7) ax_device_id = md5(fp).hex()
fp = "abc123"
exp217 = hashlib.md5(fp.encode()).hexdigest()
if ns["ax_device_id"](fp) != exp217:
    fail("ax_device_id mismatch")

# 8) msisdn normalization
cases = [
    ("081234567890", "6281234567890"),
    ("81234567890", "6281234567890"),
    ("6281234567890", "6281234567890"),
    ("08-1234-5678-90", "6281234567890"),
    ("+62 812 3456 7890", "6281234567890"),
    ("123", None),  # too short after strip
]
for inp, want in cases:
    got218 = ns["login_msisdn"](inp)
    if got218 != want:
        fail(f"login_msisdn({inp!r}) = {got218!r}, want {want!r}")

# wallet / dest
if ns["wallet_api_number"]("6281234567890") != "081234567890":
    fail("wallet_api_number 628->08")
if ns["dest_msisdn"]("081234567890") != "6281234567890":
    fail("dest_msisdn 08->62")

print("selfcheck OK")
