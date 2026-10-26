#!/usr/bin/env python3
"""Writes the deterministic HTTP policy scenarios."""
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCENARIOS = HERE / "scenarios"


def write(name: str, value: dict) -> None:
    path = SCENARIOS / f"{name}.json"
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def proxy(
    type_: str = "HTTP",
    host: str = "proxy.test",
    port: int = 8080,
    username: str = "",
    password: str = "",
    bypass=None,
) -> dict:
    return {
        "type": type_,
        "host": host,
        "port": port,
        "username": username,
        "password": password,
        "bypass": bypass or [],
    }


def local(name: str, host: str) -> None:
    write(name, {"op": "isLocalHost", "host": host})


def guard(name: str, url: str, headers) -> None:
    write(name, {"op": "guardCleartextCredentials", "url": url, "headers": headers})


def main() -> None:
    SCENARIOS.mkdir(exist_ok=True)
    for old in SCENARIOS.glob("*.json"):
        old.unlink()

    # -- isLocalHost -------------------------------------------------------------------------
    local("01-localhost", "localhost")
    local("02-localhost-uppercase", "LOCALHOST")
    local("03-loopback-ipv4", "127.0.0.1")
    local("04-private-10", "10.1.2.3")
    local("05-private-172-16", "172.16.0.1")
    local("06-private-172-31", "172.31.255.255")
    local("07-public-172-32", "172.32.0.1")
    local("08-private-192-168", "192.168.1.1")
    local("09-public-192-169", "192.169.1.1")
    local("10-link-local", "169.254.1.1")
    local("11-cgnat-100-64", "100.64.0.1")
    local("12-cgnat-100-127", "100.127.255.255")
    local("13-public-100-128", "100.128.0.1")
    local("14-unspecified-ipv4", "0.0.0.0")
    local("15-public-ipv4", "8.8.8.8")
    local("16-public-fqdn", "api.openai.com")
    local("17-dot-local", "nas.local")
    local("18-dot-lan", "foo.lan")
    local("19-dot-home", "foo.home")
    local("20-dot-internal", "foo.internal")
    local("21-dot-local-evil", "foo.local.evil")
    local("22-tailscale-dns", "machine.tailnet.ts.net")
    local("23-tailscale-apex", "ts.net")
    local("24-not-tailscale", "notts.net")
    local("25-tailscale-ula", "fd7a:115c:a1e0::1")
    local("26-tailscale-prefix", "fd7a:115c:a1e0evil")
    local("27-ipv6-brackets", "[::1]")
    local("28-empty-brackets", "[]")
    local("29-blank", "")
    local("30-whitespace", "   ")
    local("31-bare-name", "foo")
    local("32-bare-name-trailing-dot", "foo.")
    local("33-localhost-trailing-dot", "localhost.")
    local("34-plus-octet", "+127.0.0.1")
    local("35-leading-zero-octet", "127.0.0.01")
    local("36-negative-zero-octet", "-0.0.0.0")
    local("37-five-octets", "192.168.1.1.1")
    local("38-three-octets", "192.168.1")
    local("39-trailing-dot-ipv4", "192.168.1.1.")
    local("40-trailing-space-ipv4", "1.2.3.4 ")
    local("41-fullwidth-digits", "１２７.０.０.１")
    local("42-host-with-port", "127.0.0.1:80")
    local("43-host-with-path", "127.0.0.1/path")
    local("44-bare-name-with-port", "localhost:8080")
    local("45-fqdn-with-port", "api.example.com:443")

    # -- guardCleartextCredentials ------------------------------------------------------------
    auth = [["Authorization", "Bearer secret"]]
    guard("46-http-localhost-auth", "http://localhost/v1", auth)
    guard("47-http-loopback-x-api-key", "http://127.0.0.1/v1", [["X-Api-Key", "k"]])
    guard("48-http-public-auth", "http://api.example.com/v1", auth)
    guard("49-http-public-x-api-key", "http://api.example.com/v1", [["X-Api-Key", "k"]])
    guard("50-https-public-auth", "https://api.example.com/v1", auth)
    guard("51-ftp-public-auth", "ftp://api.example.com/v1", auth)
    guard("52-http-public-custom-header", "http://api.example.com/v1", [["X-Custom", "1"]])
    guard("53-http-uppercase-public-auth", "HTTP://API.EXAMPLE.COM/v1", auth)
    guard("54-http-ipv6-loopback-auth", "http://[::1]:8080/v1", auth)
    guard("55-http-registry-authority", "http://foo_bar/v1", auth)
    guard("56-http-userinfo-public-auth", "http://user:pass@api.example.com/v1", auth)
    guard("57-http-empty-authority", "http://", auth)
    guard("58-http-empty-host-path", "http:///path", auth)
    guard("59-http-public-lower-auth", "http://api.example.com/v1", [["authorization", "x"]])
    guard("60-http-public-trailing-space-key", "http://api.example.com/v1", [["Authorization ", "x"]])
    guard("61-http-public-goog-key", "http://api.example.com/v1", [["X-Goog-Api-Key", "k"]])
    guard("62-http-public-api-key", "http://api.example.com/v1", [["api-key", "k"]])
    guard("63-http-public-no-headers", "http://api.example.com/v1", [])
    guard("64-http-public-trailing-dot", "http://api.example.com./v1", auth)
    guard("65-http-unspecified-auth", "http://0.0.0.0/v1", auth)
    guard("66-http-localhost-dot-auth", "http://localhost./v1", auth)
    guard("67-http-local-evil-auth", "http://foo.local.evil/v1", auth)
    guard("68-http-malformed-escape", "http://api.example.com/%zz", auth)
    guard("69-http-malformed-host", "http://exa mple.com/v1", auth)
    guard("70-httpx-public-auth", "httpx://api.example.com/v1", auth)
    guard("71-http-tailscale-auth", "http://machine.tailnet.ts.net/v1", auth)
    guard("72-http-cgnat-auth", "http://100.64.0.1/v1", auth)
    guard("73-http-public-cgnat-auth", "http://100.128.0.1/v1", auth)
    guard("74-http-bare-name-auth", "http://foo/v1", auth)
    guard("75-http-bare-name-dot-auth", "http://foo./v1", auth)

    # -- sanitizeProxy ------------------------------------------------------------------------
    write("76-sanitize-null", {"op": "sanitizeProxy", "config": None})
    write("77-sanitize-empty-host", {"op": "sanitizeProxy", "config": proxy(host="")})
    write("78-sanitize-space-host", {"op": "sanitizeProxy", "config": proxy(host="   ")})
    write("79-sanitize-nbsp-host", {"op": "sanitizeProxy", "config": proxy(host="\u00a0")})
    write("80-sanitize-zero-width-host", {"op": "sanitizeProxy", "config": proxy(host="\u200b")})
    write("81-sanitize-port-one", {"op": "sanitizeProxy", "config": proxy(port=1)})
    write("82-sanitize-port-max", {"op": "sanitizeProxy", "config": proxy(port=65535)})
    write("83-sanitize-port-zero", {"op": "sanitizeProxy", "config": proxy(port=0)})
    write("84-sanitize-port-over", {"op": "sanitizeProxy", "config": proxy(port=65536)})
    write("85-sanitize-port-negative", {"op": "sanitizeProxy", "config": proxy(port=-1)})
    write("86-sanitize-untrimmed-host", {"op": "sanitizeProxy", "config": proxy(host=" proxy ")})
    write("87-sanitize-fields", {
        "op": "sanitizeProxy",
        "config": proxy(username="user", password="pass", bypass=["localhost", "10.0.0.0/8"]),
    })
    write("88-sanitize-socks", {"op": "sanitizeProxy", "config": proxy(type_="SOCKS")})

    # -- resolveProxy -------------------------------------------------------------------------
    write("89-resolve-null", {"op": "resolveProxy", "host": "api.example.com", "config": None})
    write("90-resolve-http", {"op": "resolveProxy", "host": "api.example.com", "config": proxy()})
    write("91-resolve-socks", {
        "op": "resolveProxy", "host": "api.example.com", "config": proxy(type_="SOCKS"),
    })
    write("92-resolve-bypass-exact", {
        "op": "resolveProxy", "host": "api.example.com",
        "config": proxy(bypass=["api.example.com"]),
    })
    write("93-resolve-bypass-wildcard", {
        "op": "resolveProxy", "host": "api.example.com",
        "config": proxy(bypass=["*.example.com"]),
    })
    write("94-resolve-bypass-cidr", {
        "op": "resolveProxy", "host": "192.168.1.5",
        "config": proxy(bypass=["192.168.0.0/16"]),
    })
    write("95-resolve-invalid-port", {
        "op": "resolveProxy", "host": "api.example.com", "config": proxy(port=0),
    })
    write("96-resolve-blank-host", {
        "op": "resolveProxy", "host": "api.example.com", "config": proxy(host=""),
    })
    write("97-resolve-blank-target", {
        "op": "resolveProxy", "host": "", "config": proxy(),
    })
    write("98-resolve-empty-bypass-entry", {
        "op": "resolveProxy", "host": "api.example.com", "config": proxy(bypass=[""]),
    })
    write("99-resolve-ipv6-bypass", {
        "op": "resolveProxy", "host": "[::1]", "config": proxy(bypass=["::1"]),
    })
    write("100-resolve-ipv6-bracket-bypass", {
        "op": "resolveProxy", "host": "[::1]", "config": proxy(bypass=["[::1]"]),
    })
    write("101-resolve-local-wildcard", {
        "op": "resolveProxy", "host": "foo.local", "config": proxy(bypass=["*.local"]),
    })
    write("102-resolve-local-wildcard-evil", {
        "op": "resolveProxy", "host": "foo.local.evil", "config": proxy(bypass=["*.local"]),
    })

    # -- isProxyBypassed ----------------------------------------------------------------------
    write("103-bypass-blank-host", {"op": "isProxyBypassed", "host": "", "bypass": ["x"]})
    write("104-bypass-empty-list", {"op": "isProxyBypassed", "host": "api.example.com", "bypass": []})
    write("105-bypass-exact", {
        "op": "isProxyBypassed", "host": "api.example.com", "bypass": ["api.example.com"],
    })
    write("106-bypass-exact-case", {
        "op": "isProxyBypassed", "host": "API.EXAMPLE.COM", "bypass": ["api.example.com"],
    })
    write("107-bypass-wildcard-apex", {
        "op": "isProxyBypassed", "host": "example.com", "bypass": ["*.example.com"],
    })
    write("108-bypass-wildcard-subdomain", {
        "op": "isProxyBypassed", "host": "api.example.com", "bypass": ["*.example.com"],
    })
    write("109-bypass-wildcard-no-dot", {
        "op": "isProxyBypassed", "host": "api.example.com", "bypass": ["*example.com"],
    })
    write("110-bypass-wildcard-tld", {
        "op": "isProxyBypassed", "host": "example.com", "bypass": ["*.com"],
    })
    write("111-bypass-wildcard-apex-tld", {
        "op": "isProxyBypassed", "host": "com", "bypass": ["*.com"],
    })
    write("112-bypass-wildcard-dot", {
        "op": "isProxyBypassed", "host": "foo.", "bypass": ["*."],
    })
    write("113-bypass-cidr-match", {
        "op": "isProxyBypassed", "host": "192.168.1.5", "bypass": ["192.168.0.0/16"],
    })
    write("114-bypass-cidr-miss", {
        "op": "isProxyBypassed", "host": "192.169.1.5", "bypass": ["192.168.0.0/16"],
    })
    write("115-bypass-cidr-zero", {
        "op": "isProxyBypassed", "host": "8.8.8.8", "bypass": ["0.0.0.0/0"],
    })
    write("116-bypass-cidr-hostname", {
        "op": "isProxyBypassed", "host": "api.example.com", "bypass": ["0.0.0.0/0"],
    })
    write("117-bypass-cidr-bits-over", {
        "op": "isProxyBypassed", "host": "192.168.1.5", "bypass": ["192.168.0.0/33"],
    })
    write("118-bypass-cidr-shape", {
        "op": "isProxyBypassed", "host": "192.168.1.5", "bypass": ["192.168.0.0/8/9"],
    })
    write("119-bypass-trimmed-entry", {
        "op": "isProxyBypassed", "host": "api.example.com", "bypass": ["  API.EXAMPLE.COM  "],
    })
    write("120-bypass-empty-entry", {
        "op": "isProxyBypassed", "host": "api.example.com", "bypass": ["", "api.example.com"],
    })
    write("121-bypass-ipv6-trim", {
        "op": "isProxyBypassed", "host": "[::1]", "bypass": ["::1"],
    })
    write("122-bypass-ipv6-bracket-entry", {
        "op": "isProxyBypassed", "host": "[::1]", "bypass": ["[::1]"],
    })
    write("123-bypass-cidr-leading-zero", {
        "op": "isProxyBypassed", "host": "192.168.1.5", "bypass": ["192.168.0.0/016"],
    })
    write("124-bypass-cidr-plus-bits", {
        "op": "isProxyBypassed", "host": "192.168.1.5", "bypass": ["192.168.0.0/+16"],
    })
    write("125-bypass-plus-octet", {
        "op": "isProxyBypassed", "host": "+192.168.0.1", "bypass": ["192.168.0.0/16"],
    })
    write("126-bypass-negative-zero", {
        "op": "isProxyBypassed", "host": "-0.0.0.0", "bypass": ["0.0.0.0/0"],
    })
    write("127-bypass-negative-bits", {
        "op": "isProxyBypassed", "host": "192.168.1.5", "bypass": ["192.168.0.0/-1"],
    })

    # -- ipv4InCidr ---------------------------------------------------------------------------
    write("128-cidr-match-16", {"op": "ipv4InCidr", "host": "192.168.1.1", "cidr": "192.168.0.0/16"})
    write("129-cidr-miss-16", {"op": "ipv4InCidr", "host": "192.169.1.1", "cidr": "192.168.0.0/16"})
    write("130-cidr-match-8", {"op": "ipv4InCidr", "host": "10.1.2.3", "cidr": "10.0.0.0/8"})
    write("131-cidr-miss-32", {"op": "ipv4InCidr", "host": "10.1.2.3", "cidr": "10.0.0.0/32"})
    write("132-cidr-match-32", {"op": "ipv4InCidr", "host": "10.1.2.3", "cidr": "10.1.2.3/32"})
    write("133-cidr-match-zero", {"op": "ipv4InCidr", "host": "10.1.2.3", "cidr": "0.0.0.0/0"})
    write("134-cidr-hostname", {"op": "ipv4InCidr", "host": "api.example.com", "cidr": "0.0.0.0/0"})
    write("135-cidr-bits-over", {"op": "ipv4InCidr", "host": "10.1.2.3", "cidr": "10.0.0.0/33"})
    write("136-cidr-bits-negative", {"op": "ipv4InCidr", "host": "10.1.2.3", "cidr": "10.0.0.0/-1"})
    write("137-cidr-shape", {"op": "ipv4InCidr", "host": "10.1.2.3", "cidr": "10.0.0.0/8/9"})
    write("138-cidr-plus-host", {"op": "ipv4InCidr", "host": "+10.1.2.3", "cidr": "10.0.0.0/8"})
    write("139-cidr-negative-zero", {"op": "ipv4InCidr", "host": "-0.1.2.3", "cidr": "0.0.0.0/0"})
    write("140-cidr-empty-bits", {"op": "ipv4InCidr", "host": "10.1.2.3", "cidr": "10.0.0.0/"})
    write("141-cidr-plus-bits", {"op": "ipv4InCidr", "host": "10.1.2.3", "cidr": "10.0.0.0/+8"})
    write("142-cidr-leading-zero-bits", {
        "op": "ipv4InCidr", "host": "10.1.2.3", "cidr": "10.0.0.0/08",
    })

    # -- proxyAuthorization -------------------------------------------------------------------
    write("143-auth-null", {"op": "proxyAuthorization", "config": None, "hasExistingHeader": False})
    write("144-auth-http", {
        "op": "proxyAuthorization",
        "config": proxy(username="user", password="pass"),
        "hasExistingHeader": False,
    })
    write("145-auth-existing", {
        "op": "proxyAuthorization",
        "config": proxy(username="user", password="pass"),
        "hasExistingHeader": True,
    })
    write("146-auth-blank-username", {
        "op": "proxyAuthorization", "config": proxy(username=""), "hasExistingHeader": False,
    })
    write("147-auth-space-username", {
        "op": "proxyAuthorization", "config": proxy(username="   "), "hasExistingHeader": False,
    })
    write("148-auth-socks", {
        "op": "proxyAuthorization",
        "config": proxy(type_="SOCKS", username="user", password="pass"),
        "hasExistingHeader": False,
    })
    write("149-auth-latin1", {
        "op": "proxyAuthorization",
        "config": proxy(username="user", password="päss"),
        "hasExistingHeader": False,
    })
    write("150-auth-unmappable", {
        "op": "proxyAuthorization",
        "config": proxy(username="user", password="€"),
        "hasExistingHeader": False,
    })
    write("151-auth-colon", {
        "op": "proxyAuthorization",
        "config": proxy(username="a:b", password="c"),
        "hasExistingHeader": False,
    })
    write("152-auth-invalid-port", {
        "op": "proxyAuthorization",
        "config": proxy(port=0, username="user", password="pass"),
        "hasExistingHeader": False,
    })
    write("153-auth-nbsp-username", {
        "op": "proxyAuthorization", "config": proxy(username="\u00a0"), "hasExistingHeader": False,
    })
    write("154-auth-zero-width-username", {
        "op": "proxyAuthorization", "config": proxy(username="\u200b"), "hasExistingHeader": False,
    })
    write("155-auth-latin1-username", {
        "op": "proxyAuthorization",
        "config": proxy(username="ü", password="p"),
        "hasExistingHeader": False,
    })
    write("156-auth-emoji", {
        "op": "proxyAuthorization",
        "config": proxy(username="user", password="\U0001f600"),
        "hasExistingHeader": False,
    })
    write("157-auth-empty-password", {
        "op": "proxyAuthorization",
        "config": proxy(username="user", password=""),
        "hasExistingHeader": False,
    })

    # -- clientPolicy -------------------------------------------------------------------------
    write("158-client-policy", {"op": "clientPolicy"})


if __name__ == "__main__":
    main()
