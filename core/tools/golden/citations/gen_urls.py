# Generates the java.net.URI fuzz corpus batches (`uri` mode) and the CitationPolicy.safeHttpUrl
# fuzz batches (`safeUrlFuzz` mode) over the same deterministic corpus.
#
# The corpus deliberately stays clear of unpaired surrogates: at the C++ boundary a lone UTF-16
# surrogate cannot survive marshalling (see the README boundary notes).
import json
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
base = os.path.join(HERE, "scenarios")
os.makedirs(base, exist_ok=True)

BATCH = 100


def w(name, body):
    with open(os.path.join(base, name + ".json"), "w") as f:
        json.dump({"name": name, **body}, f, indent=2, ensure_ascii=True)
        f.write("\n")


CURATED = [
    "http://example.com/path",
    "https://example.com",
    "https://example.com/",
    "http://example.com:80/x",
    "https://example.com:443/x",
    "http://example.com:443/x",
    "https://example.com:0/x",
    "https://example.com:65535/x",
    "https://example.com:65536/x",
    "https://example.com:00080/x",
    "http://example.com:/x",
    "http://example.com:-1/x",
    "http://example.com:+1/x",
    "HTTP://EXAMPLE.COM/Path",
    "HtTpS://Example.Com/x",
    "http://user:pw@example.com/",
    "http://@example.com/",
    "http://u@example.com/x",
    "http://%75@example.com/x",
    "http://\u00fc@example.com/x",
    "http://user:pa ss@example.com/x",
    "http://user:pw@example.com:8080/x",
    "http://127.0.0.1/x",
    "http://127.000.1.1/x",
    "http://127.0.0.256/x",
    "http://127.0.0.1.5/x",
    "http://0x7f.1/x",
    "http://0/x",
    "http://01.2.3.4/x",
    "http://[::1]:8080/x",
    "http://[::1]/x",
    "http://[fe80::1%25eth0]/x",
    "http://[fe80::1%eth0]/x",
    "http://[::1]:0/x",
    "http://[::1]:65536/x",
    "http://[/x",
    "http://[::1/x",
    "http://]/x",
    "http://exa_mple.com/",
    "http://example-.com/",
    "http://-example.com/",
    "http://example..com/",
    "http://example.com./x",
    "http://sub.domain.example/",
    "http://localhost/x",
    "http://./x",
    "http://../x",
    "http://\u00e9.example.com/",
    "http://xn--nxasmq6b.example/",
    "http://example.com/\u00e9",
    "http://example.com/\U0001f600x",
    "http://example.com/a%20b",
    "http://example.com/a%zz",
    "http://example.com/%",
    "http://example.com/%7euser",
    "http://example.com/%2F",
    "http://example.com/a//b",
    "http://example.com//",
    "http://example.com/a/./b/../c",
    "http://example.com/./",
    "http://example.com/../",
    "http://example.com/a/..",
    "http://example.com/a/.",
    "http://example.com/x?a=b+c&d=e%20f",
    "http://example.com/x?",
    "http://example.com/x?a=b c",
    "http://example.com/x?q=\u00e9",
    "http://example.com/x#f",
    "http://example.com/x#",
    "http://example.com/x#f g",
    "http://example.com?q=1",
    "http://example.com#f",
    "http:///x",
    "http://",
    "http:",
    "http:/x",
    "http:////x",
    "https:example.com",
    "mailto:a@b.com",
    "cite:turn1a1",
    "data:text/plain,hi",
    "urn:isbn:123",
    "a:b:c",
    "a:b",
    ":no-scheme",
    "1http://x",
    "ht tp://x",
    "ftp://example.com/x",
    "file:///tmp/x",
    "http+unix://%2Ftmp%2Fsock/x",
    "a1+-.:part",
    "//host/path",
    "//host",
    "///x",
    "//",
    "relative/path",
    "relative",
    "./a",
    "../a",
    "..",
    ".",
    "",
    "/",
    "/a/b",
    "?q=1",
    "#frag",
    "?",
    "#",
    "a b",
    "a\tb",
    "a\nb",
    " a ",
    "http://example.com/a b",
    "http://example.com/a|b",
    "http://example.com/a^b",
    "http://example.com/a{b}",
    "http://example.com/a\"b",
    "http://example.com/a<b>",
    "http://example.com/a\\b",
    "http://example.com/`a`",
    "http://example.com/\u0001x",
    "http://example.com/\tx",
    "http://example.com/\u007fx",
    "http://example.com/\u00a0x",
    "\u200bx://example.com/",
    "http://example.com/\u2f00x",
    "http://example.com/\ufffax",
    "http://example.com/\u2028x",
    "\ufffe",
    "\ufeffhttp://example.com/",
    "http://example.com/%c3%a9",
    "http://example.com/%C3%A9",
    "http://%65xample.com/",
    "http://example.com/;p=1",
    "http://example.com/x;y",
    "http://;@example.com/",
    "http://:80/",
    "://example.com/",
    "http://example.com:80:90/x",
    "http://foo.bar:80a/x",
    "http://example.com/a?b#c",
    "http://example.com/a#b?c",
    "http://example.com/a?b?c",
    "http://example.com/a#b#c",
]

PIECES_SCHEME = ["http", "https", "HTTP", "HtTpS", "ftp", "cite", "a1+-.", "1http", "ht tp", "htt\u00e9", ""]
PIECES_HOST = [
    "example.com",
    "EXAMPLE.com",
    "sub.example.com",
    "127.0.0.1",
    "127.00.1.1",
    "999.1.1.1",
    "exa_mple.com",
    "example-.com",
    "-example.com",
    "example..com",
    "example.com.",
    "localhost",
    "\u00e9.example.com",
    "a",
    "0",
    "",
]
PIECES_PORT = ["", ":80", ":443", ":0", ":65535", ":65536", ":-1", ":", ": 80", ":8a", ":00080", ":9" + "9" * 9]
PIECES_USERINFO = ["", "@", "u@", "u:p@", "u:p:q@", "%75@", "\u00fc@"]
PIECES_PATH = [
    "",
    "/",
    "/a",
    "/a/b",
    "/a//b",
    "/./a",
    "/../a",
    "/a/./b/../c/",
    "/a/..",
    "/%7e",
    "/%zz",
    "/%2F",
    "/a b",
    "/a|b",
    "/a^b",
    "/a{b}",
    "/\u00e4",
    "/\U0001f600",
    "/a%20b",
    "/;",
    "/a;b",
    "/:",
    "/a:b",
    "/@x",
]
PIECES_QUERY = ["", "?q=1", "?a=b&c=d", "?a=b c", "?%20", "?\u00e4=1", "?", "?q=%zz", "?a=b?c"]
PIECES_FRAG = ["", "#f", "#f g", "#\u00e4", "#", "#a#b"]
PIECES_OPAQUE = [
    "mailto:a@b.com",
    "cite:turn1a1",
    "data:text/plain,hi",
    "urn:isbn:123",
    "a:b:c",
    "a b:c",
    ":x",
    "a:\u00e9",
    "a:%zz",
    "a:/x/./y",
]
PIECES_JUNK = list("abcXYZ019:/?#[]@!$&'()*+,;=-._~%^<>\"{}|\\` \t\n\u00e9\u00e4\u03c9\U0001f600\u00a0\u2028\u2029")


def gen_random(rng, count):
    cases = []
    while len(cases) < count:
        roll = rng.random()
        if roll < 0.55:
            scheme = rng.choice(PIECES_SCHEME)
            sep = rng.choice(["://", "://", "://", ":/", ":", ":///"])
            authority = rng.choice(PIECES_USERINFO) + rng.choice(PIECES_HOST) + rng.choice(PIECES_PORT)
            raw = scheme + sep + authority + rng.choice(PIECES_PATH) + rng.choice(PIECES_QUERY) + rng.choice(PIECES_FRAG)
        elif roll < 0.7:
            raw = rng.choice(PIECES_SCHEME) + ":" + rng.choice(PIECES_OPAQUE)
        elif roll < 0.8:
            raw = "//" + rng.choice(PIECES_USERINFO) + rng.choice(PIECES_HOST) + rng.choice(PIECES_PORT) + rng.choice(PIECES_PATH)
        elif roll < 0.9:
            raw = rng.choice(PIECES_PATH) + rng.choice(PIECES_QUERY) + rng.choice(PIECES_FRAG)
        else:
            raw = rng.choice(PIECES_SCHEME) + rng.choice([":", "://"]) + rng.choice(PIECES_HOST) + rng.choice(PIECES_PORT) + rng.choice(PIECES_PATH)
        if rng.random() < 0.35 and raw:
            pos = rng.randrange(len(raw) + 1)
            raw = raw[:pos] + rng.choice(PIECES_JUNK) + raw[pos:]
        cases.append(raw)
    return cases


def main():
    rng = random.Random(20261017)
    corpus = list(CURATED) + gen_random(rng, 460)

    for index in range(0, len(corpus), BATCH):
        chunk = corpus[index : index + BATCH]
        number = "{0:02d}".format(index // BATCH)
        w("uri_batch_" + number, {"mode": "uri", "cases": chunk})
        w("safeurl_batch_" + number, {"mode": "safeUrlFuzz", "cases": chunk})


if __name__ == "__main__":
    main()
