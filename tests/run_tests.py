#!/usr/bin/env python3
"""End-to-end tests for Quartix.exe, run under Wine (or natively on Windows)."""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

EXE = os.path.abspath(sys.argv[1])
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIGN = os.path.join(ROOT, "tools", "sign_update.py")
WINE = os.environ.get("WINE", "wine")
failures = 0


def winpath(p):
    if os.name == "nt":
        return p
    return "Z:" + p.replace("/", "\\")


def run(exe, *args, data_dir, env=None):
    cmd = ([] if os.name == "nt" else [WINE]) + [exe] + list(args)
    env = dict(os.environ, QUARTIX_DATA_DIR=winpath(data_dir), WINEDEBUG="-all", **(env or {}))
    return subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=120)


def check(name, cond, detail=""):
    global failures
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else ": " + detail))
    if not cond:
        failures += 1


def sha256(path):
    return hashlib.sha256(open(path, "rb").read()).hexdigest()


def make_apk(path, lib_bytes, compress=zipfile.ZIP_STORED):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("AndroidManifest.xml", b"\0" * 64, zipfile.ZIP_DEFLATED)
        z.writestr("classes.dex", b"dex\n035\0" + b"x" * 1000, zipfile.ZIP_DEFLATED)
        z.writestr("lib/x86_64/libroblox.so", lib_bytes, compress)


def test_apk(tmp):
    data = os.path.join(tmp, "data")
    lib = b"\x7fELF" + os.urandom(300000)
    apk = os.path.join(tmp, "roblox.apk")
    make_apk(apk, lib)

    r = run(EXE, "apk-ls", winpath(apk), data_dir=data)
    check("apk-ls lists the native library", r.returncode == 0 and "lib/x86_64/libroblox.so" in r.stdout, r.stderr)

    r = run(EXE, "import-apk", winpath(apk), data_dir=data)
    check("import-apk succeeds", r.returncode == 0, r.stdout + r.stderr)
    cur = open(os.path.join(data, "apk", "current")).read().strip()
    check("current points at the APK's hash", cur == sha256(apk)[:16], cur)
    out = os.path.join(data, "apk", cur, "lib", "x86_64", "libroblox.so")
    check("extracted library is byte-identical", os.path.exists(out) and open(out, "rb").read() == lib)
    check("APK copied into the data dir", sha256(os.path.join(data, "apk", cur, "base.apk")) == sha256(apk))

    deflated = os.path.join(tmp, "deflated.apk")
    make_apk(deflated, lib, zipfile.ZIP_DEFLATED)
    r = run(EXE, "import-apk", winpath(deflated), data_dir=data)
    check("deflated library is rejected, not guessed", r.returncode != 0 and "compressed" in r.stderr, r.stderr)

    noarch = os.path.join(tmp, "arm.apk")
    with zipfile.ZipFile(noarch, "w") as z:
        z.writestr("lib/arm64-v8a/libroblox.so", b"\x7fELF")
    r = run(EXE, "import-apk", winpath(noarch), data_dir=data)
    check("APK without x86_64 library is rejected", r.returncode != 0 and "x86_64" in r.stderr, r.stderr)

    junk = os.path.join(tmp, "junk.apk")
    open(junk, "wb").write(os.urandom(5000))
    r = run(EXE, "import-apk", winpath(junk), data_dir=data)
    check("non-zip file is rejected", r.returncode != 0, r.stdout)


def build(out, pubkey, *extra):
    subprocess.run([os.environ.get("MAKE", "make"), "-s", "OUT=" + out, "UPDATE_PUBKEY=" + pubkey] + list(extra),
                   cwd=ROOT, check=True, capture_output=True)
    return out


def build_keyed(tmp):
    """Builds Quartix.exe with a throwaway signing key; returns (exe, private key file)."""
    key = os.path.join(tmp, "test.pem")
    pub = subprocess.run([sys.executable, SIGN, "keygen", key], capture_output=True, text=True, check=True).stdout.strip()
    return build(os.path.join(tmp, "keyed", "Quartix.exe"), pub), key


def sign(key, exe, version):
    subprocess.run([sys.executable, SIGN, "sign", key, exe, version], check=True, capture_output=True)
    return exe + ".sig"


def test_update(tmp):
    data = os.path.join(tmp, "data")
    login = os.path.join(data, "app", "com.roblox.client", "shared_prefs", "session.xml")
    os.makedirs(os.path.dirname(login))
    open(login, "w").write("<session>keep-me</session>")

    unkeyed = build(os.path.join(tmp, "unkeyed", "Quartix.exe"), "")
    r = run(unkeyed, "info", data_dir=data)
    check("unkeyed build reports updates disabled", "disabled" in r.stdout, r.stdout)
    staged = os.path.join(tmp, "unkeyed-new.exe")
    shutil.copy(unkeyed, staged)
    open(staged + ".sig", "w").write("quartix-update-v1\n")
    r = run(unkeyed, "apply-update", winpath(staged), winpath(staged + ".sig"), data_dir=data)
    check("unkeyed build refuses updates", r.returncode != 0 and "no update signing key" in r.stderr, r.stderr)

    keyed, key = build_keyed(tmp)
    install = os.path.join(tmp, "install")
    os.makedirs(install)
    exe = os.path.join(install, "Quartix.exe")
    shutil.copy(keyed, exe)
    before = sha256(exe)

    staged = os.path.join(tmp, "new", "Quartix.exe")
    os.makedirs(os.path.dirname(staged))
    shutil.copy(keyed, staged)
    with open(staged, "ab") as f:
        f.write(b"\0" * 512)  # a different, still valid image

    def apply(sig):
        return run(exe, "apply-update", winpath(staged), winpath(sig), data_dir=data)

    other_key = os.path.join(tmp, "other.pem")
    subprocess.run([sys.executable, SIGN, "keygen", other_key], check=True, capture_output=True)
    r = apply(sign(other_key, staged, "9.0.0"))
    check("update signed by another key is refused", r.returncode != 0 and "not valid" in r.stderr, r.stderr)

    sig = sign(key, staged, "9.0.0")
    body = open(sig).read()
    open(sig, "w").write(body.replace("version=9.0.0", "version=9.0.1"))
    r = apply(sig)
    check("tampered signature file is refused", r.returncode != 0 and "not valid" in r.stderr, r.stderr)

    r = apply(sign(key, staged, "0.1.0"))
    check("older version is refused", r.returncode != 0 and "not newer" in r.stderr, r.stderr)

    good = sign(key, staged, "9.0.0")
    with open(staged, "ab") as f:
        f.write(b"x")
    r = apply(good)
    check("file changed after signing is refused", r.returncode != 0 and "does not match" in r.stderr, r.stderr)
    check("refused updates leave the executable alone", sha256(exe) == before)

    good = sign(key, staged, "9.0.0")
    want = sha256(staged)
    r = apply(good)
    check("signed, newer update applies", r.returncode == 0, r.stdout + r.stderr)
    check("executable was replaced", sha256(exe) == want)
    check("login data survived the update", open(login).read() == "<session>keep-me</session>")

    r = run(exe, "info", data_dir=data)
    check("updated build runs", r.returncode == 0 and winpath(data).lower() in r.stdout.lower(), r.stdout + r.stderr)
    check("old executable cleaned up on next start", not os.path.exists(exe + ".old"))
    check("login data survived the restart", open(login).read() == "<session>keep-me</session>")


def make_tls_material(tmp):
    """Writes a test CA, a server cert for localhost signed by it, and a second,
    untrusted CA's localhost cert. Returns their paths."""
    import datetime
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    now = datetime.datetime.now(datetime.timezone.utc)

    def name(cn):
        return x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, cn)])

    def make_ca(cn):
        key = ec.generate_private_key(ec.SECP256R1())
        cert = (x509.CertificateBuilder().subject_name(name(cn)).issuer_name(name(cn))
                .public_key(key.public_key()).serial_number(x509.random_serial_number())
                .not_valid_before(now - datetime.timedelta(days=1))
                .not_valid_after(now + datetime.timedelta(days=30))
                .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
                .add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False), critical=True)
                .sign(key, hashes.SHA256()))
        return key, cert

    def make_leaf(ca_key, ca_cert, host, prefix):
        key = ec.generate_private_key(ec.SECP256R1())
        cert = (x509.CertificateBuilder().subject_name(name(host)).issuer_name(ca_cert.subject)
                .public_key(key.public_key()).serial_number(x509.random_serial_number())
                .not_valid_before(now - datetime.timedelta(days=1))
                .not_valid_after(now + datetime.timedelta(days=30))
                .add_extension(x509.SubjectAlternativeName([x509.DNSName(host)]), critical=False)
                .add_extension(x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
                .sign(ca_key, hashes.SHA256()))
        cert_path, key_path = os.path.join(tmp, prefix + ".crt"), os.path.join(tmp, prefix + ".key")
        open(cert_path, "wb").write(cert.public_bytes(serialization.Encoding.PEM) +
                                    ca_cert.public_bytes(serialization.Encoding.PEM))
        open(key_path, "wb").write(key.private_bytes(serialization.Encoding.PEM,
                                                     serialization.PrivateFormat.PKCS8,
                                                     serialization.NoEncryption()))
        return cert_path, key_path

    ca_key, ca_cert = make_ca("Quartix Test CA")
    ca_path = os.path.join(tmp, "ca.pem")
    open(ca_path, "wb").write(ca_cert.public_bytes(serialization.Encoding.PEM))
    rogue_key, rogue_cert = make_ca("Untrusted CA")
    return {
        "ca": ca_path,
        "good": make_leaf(ca_key, ca_cert, "localhost", "good"),
        "wrong_host": make_leaf(ca_key, ca_cert, "example.invalid", "wrong"),
        "untrusted": make_leaf(rogue_key, rogue_cert, "localhost", "rogue"),
    }


class Server:
    """Local HTTPS server mimicking GitHub's latest-release download redirects."""

    def __init__(self, tls):
        import http.server
        import socket
        import ssl
        import threading

        self.files = {}
        self.chunked = True
        files = self.files
        server = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def do_GET(self):
                if self.path.startswith("/latest/download/"):
                    name = self.path.rsplit("/", 1)[1]
                    self.send_response(302)
                    self.send_header("Location", "/assets/" + name)  # relative, like a CDN hop
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                name = self.path.rsplit("/", 1)[1]
                if not self.path.startswith("/assets/") or name not in files:
                    self.send_response(404)
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                body = files[name]
                self.send_response(200)
                if server.chunked and name.endswith(".exe"):
                    self.send_header("Transfer-Encoding", "chunked")
                    self.end_headers()
                    for i in range(0, len(body), 7000):
                        part = body[i:i + 7000]
                        self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
                    self.wfile.write(b"0\r\n\r\n")
                else:
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)

        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        self.port = s.getsockname()[1]
        s.close()
        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", self.port), Handler)
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        self.ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        self.use(tls["good"])
        self.httpd.socket = self.ctx.wrap_socket(self.httpd.socket, server_side=True)
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def use(self, cert_key):
        self.ctx.load_cert_chain(*cert_key)

    def stop(self):
        self.httpd.shutdown()


def test_online(tmp):
    tls = make_tls_material(tmp)
    srv = Server(tls)
    try:
        key = os.path.join(tmp, "k.pem")
        pub = subprocess.run([sys.executable, SIGN, "keygen", key], capture_output=True, text=True, check=True).stdout.strip()
        base = "UPDATE_BASE=https://localhost:%d/latest/download/" % srv.port
        exe_src = build(os.path.join(tmp, "b", "Quartix.exe"), pub, base, "TESTING=1")
        install = os.path.join(tmp, "install")
        os.makedirs(install)
        exe = os.path.join(install, "Quartix.exe")
        shutil.copy(exe_src, exe)
        before = sha256(exe)
        data = os.path.join(tmp, "data")
        login = os.path.join(data, "app", "com.roblox.client", "shared_prefs", "session.xml")
        os.makedirs(os.path.dirname(login))
        open(login, "w").write("<session>keep-me</session>")
        env = {"QUARTIX_TEST_CA": winpath(tls["ca"])}

        def update(*args, e=env):
            return run(exe, "update", *args, data_dir=data, env=e)

        r = update()
        check("no published release is reported, not an error", r.returncode == 0 and "no release" in r.stdout, r.stdout + r.stderr)

        new = os.path.join(tmp, "new", "Quartix.exe")
        os.makedirs(os.path.dirname(new))
        shutil.copy(exe_src, new)
        with open(new, "ab") as f:
            f.write(os.urandom(300000))  # different image, big enough for many chunks

        def publish(version, body=None, signer=key):
            sig = sign(signer, new, version)
            srv.files["Quartix.exe.sig"] = open(sig, "rb").read()
            srv.files["Quartix.exe"] = body if body is not None else open(new, "rb").read()

        publish("0.3.0")
        r = update()
        check("same version reports up to date", r.returncode == 0 and "up to date" in r.stdout, r.stdout + r.stderr)

        publish("9.0.0")
        r = update("--check")
        check("--check reports the newer version", r.returncode == 0 and "9.0.0" in r.stdout, r.stdout + r.stderr)
        check("--check does not install", sha256(exe) == before)

        r = update(e={})
        check("server outside the trusted CAs is refused", r.returncode != 0 and "certificate" in r.stderr, r.stderr)

        srv.use(tls["untrusted"])
        r = update()
        check("certificate from an untrusted CA is refused", r.returncode != 0 and "certificate" in r.stderr, r.stderr)
        srv.use(tls["wrong_host"])
        r = update()
        check("certificate for another host name is refused", r.returncode != 0 and "certificate" in r.stderr, r.stderr)
        srv.use(tls["good"])

        other = os.path.join(tmp, "other.pem")
        subprocess.run([sys.executable, SIGN, "keygen", other], check=True, capture_output=True)
        publish("9.0.0", signer=other)
        r = update()
        check("release signed by another key is refused", r.returncode != 0 and "not valid" in r.stderr, r.stderr)

        publish("9.0.0", body=open(new, "rb").read() + b"tampered")
        r = update()
        check("tampered download is refused", r.returncode != 0 and "does not match" in r.stderr, r.stderr)
        check("refused updates leave the executable alone", sha256(exe) == before)
        check("refused download is cleaned up", not os.path.exists(exe + ".download"))

        publish("9.0.0")
        r = update()
        check("signed release downloads (redirect, chunked) and installs", r.returncode == 0, r.stdout + r.stderr)
        check("executable now matches the release", sha256(exe) == sha256(new))
        check("login data survived the online update", open(login).read() == "<session>keep-me</session>")

        srv.chunked = False
        shutil.copy(exe_src, exe)
        r = update()
        check("Content-Length download also installs", r.returncode == 0 and sha256(exe) == sha256(new), r.stdout + r.stderr)
    finally:
        srv.stop()


def main():
    for t in (test_apk, test_update, test_online):
        tmp = tempfile.mkdtemp(prefix="quartix-")
        try:
            t(tmp)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
    print("%d failure(s)" % failures)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
