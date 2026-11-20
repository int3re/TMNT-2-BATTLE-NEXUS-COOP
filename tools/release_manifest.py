"""tmnt2net-latest.json of a release: what the launcher reads to know the newest version.

    python tools/release_manifest.py <tmnt2net-setup.exe> <tmnt2net-launcher.exe> <out.json>

dist/release.json holds what a person writes for each release (the version, the protocol, the date, a
title and up to four short notes for the launcher's banner, the protocol of earlier versions); this adds
the installer's and the launcher's size and SHA-256. The file goes into the GitHub release as an asset
beside tmnt2net-setup.exe: the launcher asks for .../releases/latest/download/tmnt2net-latest.json."""
import hashlib, io, json, os, re, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def describe(path):
    data = open(path, "rb").read()
    return {"name": os.path.basename(path), "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}


def main(setup, launcher, out):
    rel = json.load(io.open(os.path.join(HERE, "dist", "release.json"), encoding="utf-8"))
    version = re.search(r'TMNT2NET_VERSION "([^"]+)"', io.open(os.path.join(HERE, "src/common/version.h"), encoding="utf-8").read()).group(1)
    protocol = int(re.search(r"kVersion = (\d+)", io.open(os.path.join(HERE, "src/common/protocol.h"), encoding="utf-8").read()).group(1))
    lver = re.search(r'kLauncherVersion\[\] = L"([^"]+)"', io.open(os.path.join(HERE, "src/launcher/launcher.h"), encoding="utf-8").read()).group(1)
    assert rel["version"] == version, "dist/release.json is for %s, the source is %s" % (rel["version"], version)
    assert rel["protocol"] == protocol, "dist/release.json says protocol %s, the source %s" % (rel["protocol"], protocol)
    assert 1 <= len(rel["notes"]) <= 4 and all(len(n) <= 90 for n in rel["notes"]), "one to four notes of at most 90 characters"
    assert len(rel["title"]) <= 40, "a title of at most 40 characters"
    doc = {"schema": 1, "version": version, "protocol": protocol, "date": rel["date"], "title": rel["title"], "notes": rel["notes"],
           "setup": describe(setup), "launcher": dict(describe(launcher), version=lver), "protocols": rel["protocols"]}
    io.open(out, "w", encoding="utf-8", newline="\n").write(json.dumps(doc, ensure_ascii=False, indent=2) + "\n")
    print("written:", out)
    print("  mod %s (protocol %d), setup %d bytes %s" % (version, protocol, doc["setup"]["size"], doc["setup"]["sha256"]))
    print("  launcher %s, %d bytes %s" % (lver, doc["launcher"]["size"], doc["launcher"]["sha256"]))


if __name__ == "__main__":
    main(*sys.argv[1:4])
