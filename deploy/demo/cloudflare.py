#!/usr/bin/env python3
"""Cloudflare for the hosted demo (docs/31-hosted-demo.md): DNS records, the origin certificate, Access.

Idempotent; prints no secret. The API token is read from a file (default
~/.config/fjarr/cloudflare-api-token), scoped to the fjarr.io zone and its account.

  cloudflare.py dns <host-ip>                   signal + turn records (demo only with `access`)
  cloudflare.py origin-cert root@<host>         Origin CA certificate; its key is made on the server
  cloudflare.py access <host-ip> <email>...     Access for demo.fjarr.io (email one-time PIN), then its record
"""
import json
import os
import subprocess
import sys
import urllib.error
import urllib.request

ZONE_NAME = "fjarr.io"
API = "https://api.cloudflare.com/client/v4"
TOKEN = open(os.path.expanduser(os.environ.get("CF_TOKEN_FILE", "~/.config/fjarr/cloudflare-api-token"))).read().strip()


def api(method, path, body=None):
    req = urllib.request.Request(API + path, method=method, data=json.dumps(body).encode() if body is not None else None,
                                 headers={"Authorization": f"Bearer {TOKEN}", "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req) as r:
            data = json.load(r)
    except urllib.error.HTTPError as e:
        data = json.load(e)
    if not data.get("success"):
        raise SystemExit(f"{method} {path}: {data.get('errors')}")
    return data["result"]


def zone():
    z = api("GET", f"/zones?name={ZONE_NAME}")[0]
    return z["id"], z["account"]["id"]


def record(zone_id, name, ip, proxied):
    existing = api("GET", f"/zones/{zone_id}/dns_records?type=A&name={name}")
    body = {"type": "A", "name": name, "content": ip, "proxied": proxied, "ttl": 1,
            "comment": "fjarr hosted demo (docs/31)"}
    if existing:
        api("PUT", f"/zones/{zone_id}/dns_records/{existing[0]['id']}", body)
        print(f"updated {name} -> {ip} ({'proxied' if proxied else 'dns only'})")
    else:
        api("POST", f"/zones/{zone_id}/dns_records", body)
        print(f"created {name} -> {ip} ({'proxied' if proxied else 'dns only'})")


def cmd_dns(ip):
    zone_id, _ = zone()
    record(zone_id, "signal.fjarr.io", ip, True)
    record(zone_id, "turn.fjarr.io", ip, False)  # coturn: Cloudflare's proxy carries no UDP


def cmd_origin_cert(host):
    # The key is generated on the server and never leaves it; only the CSR comes here.
    csr = subprocess.run(["ssh", "-o", "BatchMode=yes", host,
                          "set -e; cd /opt/fjarr/certs; umask 077;"
                          "[ -f origin.key ] || openssl ecparam -name prime256v1 -genkey -noout -out origin.key;"
                          "openssl req -new -key origin.key -subj /CN=fjarr-demo"],
                         check=True, capture_output=True, text=True).stdout
    cert = api("POST", "/certificates", {"hostnames": ["demo.fjarr.io", "signal.fjarr.io"],
                                         "requested_validity": 5475, "request_type": "origin-ecc", "csr": csr})
    subprocess.run(["ssh", "-o", "BatchMode=yes", host, "umask 077; cat > /opt/fjarr/certs/origin.pem"],
                   input=cert["certificate"], check=True, text=True)
    print(f"origin certificate for {', '.join(cert['hostnames'])}, valid until {cert['expires_on']}")


def cmd_access(ip, emails):
    zone_id, account = zone()
    # Email one-time PIN as a login method, if the account has none of that kind yet.
    idps = api("GET", f"/accounts/{account}/access/identity_providers")
    if not any(p["type"] == "onetimepin" for p in idps):
        api("POST", f"/accounts/{account}/access/identity_providers", {"name": "One-time PIN", "type": "onetimepin", "config": {}})
        print("enabled the one-time PIN login")
    apps = [a for a in api("GET", f"/accounts/{account}/access/apps") if a.get("domain") == "demo.fjarr.io"]
    policy = {"name": "fjarr demo operators", "decision": "allow",
              "include": [{"email": {"email": e}} for e in emails]}
    if apps:
        app = apps[0]
        for p in api("GET", f"/accounts/{account}/access/apps/{app['id']}/policies"):
            api("DELETE", f"/accounts/{account}/access/apps/{app['id']}/policies/{p['id']}")
        print("updating the Access application for demo.fjarr.io")
    else:
        app = api("POST", f"/accounts/{account}/access/apps",
                  {"name": "fjarr demo", "domain": "demo.fjarr.io", "type": "self_hosted", "session_duration": "24h",
                   "app_launcher_visible": False})
        print("created the Access application for demo.fjarr.io")
    api("POST", f"/accounts/{account}/access/apps/{app['id']}/policies", {**policy, "precedence": 1})
    print(f"allowed: {', '.join(emails)}")
    # Only now the record: before Access, anyone with the URL would have been given a grant.
    record(zone_id, "demo.fjarr.io", ip, True)


if __name__ == "__main__":
    args = sys.argv[1:]
    if args[:1] == ["dns"] and len(args) == 2:
        cmd_dns(args[1])
    elif args[:1] == ["origin-cert"] and len(args) == 2:
        cmd_origin_cert(args[1])
    elif args[:1] == ["access"] and len(args) >= 3:
        cmd_access(args[1], args[2:])
    else:
        raise SystemExit(__doc__)
