#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Libeccio: the web installer site (GitHub Pages) for one built package.

  web/libeccio_site.py PACKAGE.fwsc VERSION OUT_DIR

Melodee's installer (make_site.py, installer only: no manual, no editor) carrying PACKAGE, plus an
index.html that opens it. CI runs this after each build of main and deploys OUT_DIR to Pages.
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import make_site  # noqa: E402


def main(pkg, version, out):
    out = Path(out)
    make_site.main(pkg, version, out, installer_only=True)
    page = out / "webapp" / "installer" / "index.html"
    html = page.read_text(encoding="utf-8")
    html = re.sub(r"<title>.*?</title>", f"<title>Libeccio installer {version}</title>", html, count=1, flags=re.S)
    # the package's name where the installer shows it; Melodee's credits, its own update mode and the rest stay
    for was, now in (('<a class="brand" href="../editor/">Melodee</a>', '<span class="brand">Libeccio</span>'),
                     ("Install Melodee", "Install Libeccio"), ("Installing Melodee", "Installing Libeccio"),
                     ("Melodee {v}", "Libeccio {v}"), ("Melodee をインストール", "Libeccio をインストール"),
                     ("Melodee インストール中", "Libeccio インストール中")):
        html = html.replace(was, now)
    # no editor or manual here: Melodee's (Libeccio keeps its editor protocol and everything its manual covers)
    html = html.replace('href="../editor/"', 'href="https://keremimo.github.io/melodee/webapp/editor/"')
    html = html.replace('href="../manual/"', 'href="https://keremimo.github.io/melodee/webapp/manual/"')
    html = html.replace("Melodee by Ellic Studio", "Libeccio (github.com/StickmanFpv/libeccio), a fork of Melodee by Ellic Studio", 1)
    page.write_text(html, encoding="utf-8")
    (out / "index.html").write_text(
        '<!doctype html><meta charset="utf-8"><title>Libeccio</title>'
        '<meta http-equiv="refresh" content="0; url=webapp/installer/">'
        '<a href="webapp/installer/">Libeccio installer</a>\n', encoding="utf-8")
    (out / ".nojekyll").write_text("")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:4])
