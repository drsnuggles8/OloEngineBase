#!/usr/bin/env python3
"""Fail if a workflow or action downloads a file without retrying every error.

curl's --retry repeats a transfer only after a timeout or an HTTP 408, 429 or 5xx.
A connection reset in the middle of the body (curl exit 56) is not on that list, and
it is how a large download from a release CDN fails. A step that says `--retry 3`
therefore fails on the first reset with its three retries unused. --retry-all-errors
makes --retry cover it, and does nothing without a --retry count above zero.

A retry restarts the transfer. With -o curl truncates the file first; on stdout (a
pipe, a `>` redirection, a `$(...)`) the bytes already written are written again.

Invoke-WebRequest gives up on the first dropped connection, and its
-MaximumRetryCount answers HTTP status codes only. A file download in PowerShell
uses curl.exe.

The rules, for every command of every `run:` script in a tracked YAML file under
.github/:

  * a curl command that writes a file (-o / -O) carries `--retry <n>` with n above
    zero and `--retry-all-errors`. The count is the last --retry of the command,
    which is the one curl uses;
  * a curl command with -f / --fail and no -o is a download written to stdout,
    which cannot be retried safely: it goes to a file first;
  * `--retry-all-errors` is not used on a curl command without -o;
  * no `Invoke-WebRequest ... -OutFile`.

A curl command with neither -f nor -o (a page piped to grep) is left alone.

The scripts are read through a YAML parser, so a folded scalar (`run: >`) is judged
as the one line the shell receives. A script is split into commands the way a shell
splits it: quotes hold a `;` or a `#` together, a comment is not an argument, and a
word that says `curl` inside another command's argument is not a download. Each
command is judged on its own arguments.

Not read, because the text of the step does not hold it: the body of a here-document,
a script passed as one quoted word (`bash -c`), options that arrive through a
variable or a curl config file, a function that wraps curl. A --retry count that is
not a literal number (a variable) is taken as given.

Opt out with a comment on one of the command's own lines:

    # OLO_DOWNLOAD_NO_RETRY_OK: <reason>
"""

from __future__ import annotations

import re
import subprocess
import sys
from dataclasses import dataclass, field

try:
    import yaml
except ImportError:  # no silent pass: a hook that cannot read YAML checks nothing
    print("check_ci_download_retries.py needs PyYAML (pip install pyyaml); pre-commit installs it.")
    sys.exit(1)

OPT_OUT = "OLO_DOWNLOAD_NO_RETRY_OK:"

NO_ALL_ERRORS = "curl download without --retry-all-errors; --retry alone does not repeat a connection reset"
NO_COUNT = "curl download with --retry-all-errors whose last --retry count is missing or zero; the flag alone enables no retry"
STDOUT_FAIL = (
    "curl -f download written to stdout cannot be retried safely; "
    "download to a file with -o, --retry <n> and --retry-all-errors"
)
STDOUT_RETRY = "--retry-all-errors on a curl transfer written to stdout: a retry writes the bytes again; use -o"
IWR = "Invoke-WebRequest -OutFile does not retry a dropped connection; use curl.exe with --retry <n> --retry-all-errors"

# Words a shell reads before the command itself.
PREFIX_WORDS = {"if", "then", "else", "elif", "do", "while", "until", "!", "time", "exec", "command", "sudo", "env", "try"}
# Programs that run the command in their arguments. What they run is judged from the
# first word that names curl; their own options are not parsed.
WRAPPERS = {"timeout", "nice", "ionice", "nohup", "stdbuf", "xargs"}
CURL = ("curl", "curl.exe")
ASSIGNMENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*=.*", re.S)
# After a backslash these are escaped; before anything else the backslash is a
# character of the word (a Windows path in a PowerShell script).
ESCAPABLE = " \t\"'\\;|&(){}#$`<>"
# curl's options that take a value, from `curl --help all` of 8.12 and 8.22: the word
# after one is its value, not an option and not a URL, and what follows a short one
# in a cluster is its value. An option missing here (a later curl's) is read as a
# flag, and its value then counts as a URL: that can add a finding, never hide one.
SHORT_WITH_VALUE = set("ACDEFHKPQTUXYbcdehmortuwxyz")
LONG_WITH_VALUE = frozenset(
    """
    abstract-unix-socket alt-svc aws-sigv4 cacert capath cert cert-type ciphers config
    connect-timeout connect-to continue-at cookie cookie-jar create-file-mode crlfile curves data
    data-ascii data-binary data-raw data-urlencode delegation dns-interface dns-ipv4-addr
    dns-ipv6-addr dns-servers doh-url dump-header ech egd-file engine etag-compare etag-save
    expect100-timeout form form-string ftp-account ftp-alternative-to-user ftp-method ftp-port
    ftp-ssl-ccc-mode happy-eyeballs-timeout-ms haproxy-clientip header help hostpubmd5 hostpubsha256
    hsts interface ip-tos ipfs-gateway json keepalive-cnt keepalive-time key key-type knownhosts krb
    libcurl limit-rate local-port login-options mail-auth mail-from mail-rcpt max-filesize
    max-redirs max-time netrc-file noproxy oauth2-bearer output output-dir parallel-max
    parallel-max-host pass pinnedpubkey preproxy proto proto-default proto-redir proxy proxy-cacert
    proxy-capath proxy-cert proxy-cert-type proxy-ciphers proxy-crlfile proxy-header proxy-key
    proxy-key-type proxy-pass proxy-pinnedpubkey proxy-service-name proxy-tls13-ciphers
    proxy-tlsauthtype proxy-tlspassword proxy-tlsuser proxy-user proxy1.0 pubkey quote random-file
    range rate referer request request-target resolve retry retry-delay retry-max-time sasl-authzid
    service-name sigalgs socks4 socks4a socks5 socks5-gssapi-service socks5-hostname speed-limit
    speed-time ssl-sessions stderr telnet-option tftp-blksize time-cond tls-max tls13-ciphers
    tlsauthtype tlspassword tlsuser trace trace-ascii trace-config unix-socket upload-file
    upload-flags url url-query user user-agent variable vlan-priority write-out
    """.split()
)


@dataclass
class Command:
    start: int  # index of the script line its first word is on
    end: int = 0
    words: list[str] = field(default_factory=list)


class _Reader:
    """Splits a script into commands, as far as a download check needs a shell."""

    def __init__(self, script: str) -> None:
        self.s = script
        self.i = 0
        self.line = 0
        self.commands: list[Command] = []
        self.heredocs: list[str] = []

    def read(self, closer: str = "") -> None:
        s, n = self.s, len(self.s)
        current: Command | None = None
        word: str | None = None
        target_follows = False  # the next word is a redirection's target, not an argument

        def add(text: str) -> None:
            nonlocal current, word
            if current is None:
                current = Command(self.line)
            word = text if word is None else word + text

        def end_word() -> None:
            nonlocal word, target_follows
            if word is not None and current is not None:
                if target_follows:
                    target_follows = False
                else:
                    current.words.append(word)
            word = None

        def end_command() -> None:
            nonlocal current, target_follows
            end_word()
            target_follows = False
            if current is not None and current.words:
                current.end = self.line
                self.commands.append(current)
            current = None

        while self.i < n:
            c = s[self.i]
            nxt = s[self.i + 1] if self.i + 1 < n else ""
            if c == "\\":
                if nxt == "\n":  # a continued line
                    self.i += 2
                    self.line += 1
                elif nxt and nxt in ESCAPABLE:
                    add(nxt)
                    self.i += 2
                else:
                    add(c)
                    self.i += 1
            elif c == "`":
                j = self.i + 1
                while j < n and s[j] in " \t":
                    j += 1
                if j < n and s[j] == "\n":  # PowerShell's continued line
                    self.i = j + 1
                    self.line += 1
                else:  # a command substitution, or an escape this check does not need
                    end_command()
                    self.i += 1
            elif c == "'":
                j = s.find("'", self.i + 1)
                j = n if j < 0 else j
                add(s[self.i + 1 : j])
                self.line += s.count("\n", self.i, j)
                self.i = j + 1
            elif c == '"':
                self.i += 1
                add("")
                while self.i < n and s[self.i] != '"':
                    d = s[self.i]
                    e = s[self.i + 1] if self.i + 1 < n else ""
                    if d == "\\" and e == "\n":
                        self.line += 1
                        self.i += 2
                    elif d == "\\" and e and e in '"\\$`':
                        add(e)
                        self.i += 2
                    elif d == "$" and e == "(":  # still a command, inside the quotes
                        self.i += 2
                        self.read(")")
                        add("$()")
                    else:
                        if d == "\n":
                            self.line += 1
                        add(d)
                        self.i += 1
                self.i += 1
            elif c == "$" and nxt == "(":
                self.i += 2
                self.read(")")
                add("$()")
            elif c == "$" and nxt == "{":  # ${name} and ${{ expression }} are part of a word
                depth, j = 0, self.i + 1
                while j < n:
                    if s[j] == "{":
                        depth += 1
                    elif s[j] == "}":
                        depth -= 1
                        if depth == 0:
                            break
                    j += 1
                add(s[self.i : j + 1])
                self.i = j + 1
            elif c == "#" and word is None:  # a comment starts a word, never ends one
                j = s.find("\n", self.i)
                self.i = n if j < 0 else j
            elif s.startswith("<<<", self.i):  # a here-string supplies one word
                end_word()
                target_follows = True
                self.i += 3
            elif c == "<" and nxt == "<":
                match = re.match(r"<<-?[ \t]*(['\"]?)([A-Za-z_][A-Za-z0-9_]*)\1", s[self.i :])
                if match:
                    self.heredocs.append(match.group(2))
                    self.i += match.end()
                else:
                    add(c)
                    self.i += 1
            elif c == "&" and nxt == ">":  # bash's `&>file`: a redirection, not the `&` that ends a command
                end_word()
                self.i += 1
            elif c in "<>":
                if word is not None and (word.isdigit() or word == "*"):
                    word = None  # the descriptor of `2>`, or PowerShell's `*>`
                end_word()
                self.i += 1
                while self.i < n and s[self.i] in ">|":
                    self.i += 1
                if self.i < n and s[self.i] == "&":  # `>&2`: the descriptor is the target
                    self.i += 1
                target_follows = True
            elif c in " \t":
                end_word()
                self.i += 1
            elif c == "\n":
                end_command()
                self.line += 1
                self.i += 1
                for delimiter in self.heredocs:  # the bodies follow the line that opened them
                    while self.i < n:
                        j = s.find("\n", self.i)
                        j = n if j < 0 else j
                        body_line = s[self.i : j].strip()
                        self.i = min(j + 1, n)
                        self.line += 1
                        if body_line == delimiter:
                            break
                self.heredocs = []
            elif c == ")" and closer == ")":
                end_command()
                self.i += 1
                return
            elif c in ";|&(){}":
                end_command()
                self.i += 1
            else:
                add(c)
                self.i += 1
        end_command()


def split_commands(script: str) -> list[Command]:
    reader = _Reader(script)
    reader.read()
    return reader.commands


def program_name(word: str) -> str:
    """Lower-cased, without its directory."""
    return re.split(r"[\\/]", word)[-1].lower()


def executable(words: list[str]) -> tuple[str, list[str]]:
    """The command's program, lower-cased without its directory, and its arguments."""
    index = 0
    while index < len(words):
        word = words[index]
        if word in PREFIX_WORDS or ASSIGNMENT_RE.fullmatch(word):
            index += 1
        elif word.startswith("-") and index > 0 and words[index - 1] in ("sudo", "env"):
            index += 1
        elif word.startswith("$") and index + 1 < len(words) and words[index + 1] == "=":
            index += 2  # PowerShell: $result = command ...
        else:
            program = program_name(word)
            if program in WRAPPERS:
                for later in range(index + 1, len(words)):
                    if program_name(words[later]) in CURL:
                        return program_name(words[later]), words[later + 1 :]
            return program, words[index + 1 :]
    return "", []


@dataclass
class _Operation:
    """What one curl operation was told, each option as its last occurrence left it."""

    fail: bool = False
    fail_with_body: bool = False
    all_errors: bool = False
    remote_name_all: bool = False
    retry: str = ""  # the last --retry value
    slots: list[bool] = field(default_factory=list)  # one per -o / -O, in order: False is `-o -`
    not_remote: set[int] = field(default_factory=set)  # URLs --no-remote-name took out of --remote-name-all
    urls: int = 0  # transfers

    def option(self, name: str, value: str) -> None:
        if name in ("f", "fail"):
            self.fail = True
        elif name == "no-fail":
            self.fail = False
        elif name == "fail-with-body":
            self.fail_with_body = True
        elif name == "no-fail-with-body":
            self.fail_with_body = False
        elif name == "retry-all-errors":
            self.all_errors = True
        elif name == "no-retry-all-errors":
            self.all_errors = False
        elif name == "retry":
            self.retry = value
        elif name in ("o", "output"):
            self.slots.append(value != "-")
        elif name in ("O", "remote-name"):
            self.slots.append(True)
        elif name == "no-remote-name":  # marks the first URL without a destination; a later -o still serves it
            self.not_remote.add(len(self.slots))
        elif name == "remote-name-all":
            self.remote_name_all = True
        elif name == "no-remote-name-all":
            self.remote_name_all = False
        elif name == "url":
            self.urls += 1

    def findings(self) -> list[str]:
        # curl pairs destinations with URLs in order. A URL beyond them is named after
        # its remote file under --remote-name-all, unless --no-remote-name marked it,
        # and goes to stdout otherwise. True is a file.
        if self.urls:
            destinations = [
                self.slots[i] if i < len(self.slots) else (self.remote_name_all and i not in self.not_remote)
                for i in range(self.urls)
            ]
        else:  # the URLs are not in the text (xargs): every destination given counts
            destinations = self.slots + ([True] if self.remote_name_all else [])
        to_file = any(destinations)
        to_stdout = not all(destinations)
        found = []
        if to_stdout and self.all_errors:
            found.append(STDOUT_RETRY)
        elif to_stdout and (self.fail or self.fail_with_body):
            found.append(STDOUT_FAIL)
        if to_file and not self.all_errors:
            found.append(NO_ALL_ERRORS)
        # curl keeps the last --retry it is given: `--retry 3 --retry 0` retries nothing.
        elif to_file and (not self.retry or (self.retry.isdigit() and int(self.retry) == 0)):
            found.append(NO_COUNT)
        return found


def curl_findings(args: list[str]) -> list[str]:
    """Reads the arguments in order, as curl does, and judges each operation on its own.

    `--next`, or `:` in a cluster of short options, starts a new operation with the
    options reset. It is one only where curl reads an option: as the value of
    another option (`--header --next`) or after `--` it is text.
    """
    found: list[str] = []
    operation = _Operation()
    index = 0
    options_ended = False
    while index < len(args):
        arg = args[index]
        index += 1
        if options_ended:
            operation.urls += 1
        elif arg == "--":
            options_ended = True
        elif arg.startswith("--"):
            name, equals, attached = arg[2:].partition("=")
            # --expand-output is --output with curl's variables expanded in the value.
            name = name.removeprefix("expand-")
            if name == "next":
                found.extend(operation.findings())
                operation = _Operation()
            elif name in LONG_WITH_VALUE:
                if equals:  # --output=file, curl 8.16 and later
                    operation.option(name, attached)
                else:
                    operation.option(name, args[index] if index < len(args) else "")
                    index += 1
            else:
                operation.option(name, "")
        elif arg.startswith("-") and len(arg) > 1:
            for position, letter in enumerate(arg[1:], start=1):
                if letter == ":":  # -: is --next, alone or in a cluster (-s:)
                    found.extend(operation.findings())
                    operation = _Operation()
                elif letter in SHORT_WITH_VALUE:
                    attached = arg[position + 1 :]  # -oarchive.tar.gz
                    if attached:
                        operation.option(letter, attached)
                    else:
                        operation.option(letter, args[index] if index < len(args) else "")
                        index += 1
                    break
                else:
                    operation.option(letter, "")
        else:
            operation.urls += 1
    found.extend(operation.findings())
    return found


def command_findings(words: list[str]) -> list[str]:
    """What is wrong with one command, judged on its own arguments."""
    program, args = executable(words)
    if program in CURL:
        return curl_findings(args)
    if program in ("invoke-webrequest", "iwr") and any(arg.lower().startswith("-outf") for arg in args):
        return [IWR]
    return []


def script_findings(script: str) -> list[tuple[int, str]]:
    """(index of the script line, message) for every non-compliant command."""
    opted_out = {index for index, line in enumerate(script.split("\n")) if OPT_OUT in line}
    found = []
    for command in split_commands(script):
        if any(line in opted_out for line in range(command.start, command.end + 1)):
            continue
        for message in command_findings(command.words):
            found.append((command.start, message))
    return found


def run_scripts(text: str) -> list[tuple[int, bool, str]]:
    """Every `run:` scalar of a YAML document: (1-based line, is literal block, script)."""
    scripts = []

    def walk(node: yaml.Node) -> None:
        if isinstance(node, yaml.MappingNode):
            for key, value in node.value:
                if isinstance(key, yaml.ScalarNode) and key.value == "run" and isinstance(value, yaml.ScalarNode):
                    scripts.append((value.start_mark.line + 1, value.style == "|", value.value))
                else:
                    walk(value)
        elif isinstance(node, yaml.SequenceNode):
            for item in node.value:
                walk(item)

    root = yaml.compose(text, Loader=yaml.SafeLoader)
    if root is not None:
        walk(root)
    return scripts


def file_findings(text: str) -> list[tuple[int, str]]:
    """(1-based line in the file, message). A folded or one-line script reports its `run:` line."""
    found = []
    for line, literal, script in run_scripts(text):
        for index, message in script_findings(script):
            # A literal block keeps its lines, the first one after the `|`.
            found.append((line + 1 + index if literal else line, message))
    return found


# What the rules must catch and must leave alone, each with the one message expected.
# Checked on every run, so a change that stops a rule matching fails here, not in
# silence.
SCRIPTS_MUST_FAIL = [
    ('curl -fL --retry 3 --retry-delay 5 -o "${work}/${tarball}" "$url"', NO_ALL_ERRORS),
    ("curl.exe --fail --silent --show-error --location --output $archive $url", NO_ALL_ERRORS),
    ("curl -fSL --retry 3 --max-time 120 \\\n  -o sample.mp4 https://example.invalid/sample.mp4", NO_ALL_ERRORS),
    ("Invoke-WebRequest -Uri $url -OutFile mesa3d.7z", IWR),
    ("$response = Invoke-WebRequest -Uri $url -OutFile mesa3d.7z", IWR),
    # the flag enables nothing without a count, or with a count of zero
    ('curl -fL --retry-all-errors -o "$file" "$url"', NO_COUNT),
    ('curl -fL --retry 0 --retry-all-errors -o "$file" "$url"', NO_COUNT),
    # curl keeps the last count it is given
    ('curl -fL --retry 3 --retry 0 --retry-all-errors -o "$file" "$url"', NO_COUNT),
    # the first command's flags do not vouch for the second
    ('curl -fL --retry 3 --retry-all-errors -o a "$a"; curl -fL --retry 3 -o b "$b"', NO_ALL_ERRORS),
    ('curl -fL --retry 3 --retry-all-errors -o a "$a" && curl -fL -o b "$b"', NO_ALL_ERRORS),
    # a flag in a comment is not an argument
    ('curl -fL --retry 3 -o file "$url" # --retry-all-errors', NO_ALL_ERRORS),
    # a quoted `;` does not end the command
    ('curl "https://example.invalid/a;b" -fL -o file', NO_ALL_ERRORS),
    # a file download is judged with or without -f
    ('curl -L -o file "$url"', NO_ALL_ERRORS),
    ('curl -LO "$url"', NO_ALL_ERRORS),
    # stdout cannot be retried: a redirection, a pipe, a substitution
    ('curl -fL --retry 3 --retry-all-errors "$url" > archive', STDOUT_RETRY),
    ('curl -sL --retry 3 --retry-all-errors "$url" | tar -xz', STDOUT_RETRY),
    ('curl -fsSL "$url" | sudo tee /etc/key > /dev/null', STDOUT_FAIL),
    ('key="$(curl -fsSL "$url")"', STDOUT_FAIL),
    ("sudo /usr/bin/curl -fL -o file https://example.invalid/file", NO_ALL_ERRORS),
    # an option's value is not an option
    ("curl --retry 3 --header '--retry-all-errors' -o f \"$url\"", NO_ALL_ERRORS),
    # the later of two opposite flags is the one in force
    ('curl -L -o f --retry 3 --retry-all-errors --no-retry-all-errors "$url"', NO_ALL_ERRORS),
    # `-` as a destination is stdout, in either spelling
    ('curl --retry 3 --retry-all-errors --output=- "$url" > archive', STDOUT_RETRY),
    ('curl --retry 3 --retry-all-errors -o - "$url" > archive', STDOUT_RETRY),
    # a value attached to -o may hold any character
    ('curl -L -oarchive.tar.gz "$url"', NO_ALL_ERRORS),
    # each -o serves one URL: the second transfer here goes to stdout
    ('curl --retry 3 --retry-all-errors -o a "$a" "$b" > b', STDOUT_RETRY),
    # --next starts an operation with the options reset: the second transfer has no destination
    ('curl --retry 3 --retry-all-errors --remote-name-all "$a" --next --retry 3 --retry-all-errors "$b" > b', STDOUT_RETRY),
    ('curl --retry 3 --retry-all-errors -o a "$a" --next -o b "$b"', NO_ALL_ERRORS),
    # `:` in a cluster of short options is --next too
    ('curl --retry 3 --retry-all-errors --remote-name-all "$a" -s: --retry 3 --retry-all-errors "$b" > b', STDOUT_RETRY),
    # --next as the value of another option is text: this is one operation, and its second URL has no -o
    ('curl --retry 3 --retry-all-errors -o a --header --next "$a" "$b" > b', STDOUT_RETRY),
    # and after `--` it is a URL
    ('curl --retry 3 --retry-all-errors -o a -- "$a" --next > b', STDOUT_RETRY),
    # `&>log` is a redirection, not the `&` that ends a command
    ('curl -L &>download.log -o archive "$url"', NO_ALL_ERRORS),
    # and a lone `&` still ends one
    ('curl -fL --retry 3 --retry-all-errors -o a "$a" & curl -fL -o b "$b"', NO_ALL_ERRORS),
    # --no-remote-name takes one URL out of --remote-name-all: the first without a destination
    ('curl --retry 3 --retry-all-errors --remote-name-all --no-remote-name "$url" > archive', STDOUT_RETRY),
    ('curl --retry 3 --retry-all-errors --remote-name-all "$a" --no-remote-name "$b" > a', STDOUT_RETRY),
    # --remote-name-all can be switched off again
    ('curl --retry 3 --retry-all-errors --remote-name-all --no-remote-name-all "$url" > archive', STDOUT_RETRY),
    # --expand-output is --output
    ('curl -L --expand-output "{{name}}.tar.gz" "$url"', NO_ALL_ERRORS),
    # --fail-with-body is not switched off by --no-fail
    ('curl -sSL --fail-with-body --no-fail "$url" | tar -xz', STDOUT_FAIL),
    # a program that runs curl does not hide it
    ('timeout -k 5 300 curl -fL -o f "$url"', NO_ALL_ERRORS),
    ('cat urls.txt | xargs -n 1 curl -fLO', NO_ALL_ERRORS),
    # a quoted `>` is an argument, here the value of --write-out
    ("curl --retry 3 --write-out '>status' -o archive \"$url\"", NO_ALL_ERRORS),
    # a here-string is one word and hides nothing after it
    ('cat <<<EOF\ncurl -L -o f "$url"', NO_ALL_ERRORS),
    ("if ($true) { C:\\Windows\\System32\\curl.exe --fail --output f $url }", NO_ALL_ERRORS),
]
SCRIPTS_MUST_PASS = [
    'curl -fL --retry 3 --retry-delay 5 --retry-all-errors -o "${work}/${tarball}" "$url"',
    "curl -fSL --retry 3 --retry-all-errors --max-time 120 \\\n  -o sample.mp4 https://example.invalid/sample.mp4",
    "curl.exe --fail --retry 3 --retry-all-errors --output mesa3d.7z $url",
    "curl.exe --fail --retry=5 --retry-all-errors --output mesa3d.7z $url",
    "curl.exe --fail --retry 3 `\n  --retry-all-errors --output mesa3d.7z $url",
    'curl -fL --retry 0 --retry 3 --retry-all-errors -o "$file" "$url"',
    'curl -fL --retry "$RETRIES" --retry-all-errors -o "$file" "$url"',
    'curl -fL --retry 3 --retry-all-errors -o a "$a"; curl -fL --retry 3 --retry-all-errors -o b "$b"',
    'curl -fsSLo "$file" --retry 3 --retry-all-errors "$url"',
    "curl --retry 3 --header 'X-A: --no-retry-all-errors' --retry-all-errors -o f \"$url\"",
    'curl -L -o f --retry 3 --no-retry-all-errors --retry-all-errors "$url"',
    'curl --retry 3 --retry-all-errors --output=archive "$url"',
    'curl -L --retry 3 --retry-all-errors -oarchive.tar.gz "$url"',
    'curl --retry 3 --retry-all-errors -o a "$a" -o b "$b"',
    'curl --retry 3 --retry-all-errors -o a "$a" --next --retry 3 --retry-all-errors -o b "$b"',
    "curl --retry 3 --retry-all-errors --write-out '>status' -o archive \"$url\"",
    'curl --retry 3 --retry-all-errors -o a "$a" -s: --retry 3 --retry-all-errors -o b "$b"',
    'curl --retry 3 --retry-all-errors -o a --header --next "$a"',
    'curl --retry 3 --retry-all-errors --expand-output "{{name}}.tar.gz" "$url"',
    'timeout 300 curl -fL --retry 3 --retry-all-errors -o f "$url"',
    "timeout 300 make test-curl",
    'curl -L --retry 3 --retry-all-errors &>download.log -o archive "$url"',
    'curl -L --retry 3 --retry-all-errors -o archive "$url" &>> download.log',
    "curl.exe --fail --retry 3 --retry-all-errors --output f $url *> log.txt",
    # a destination given after --no-remote-name still serves the URL, and -O before it already did
    'curl --retry 3 --retry-all-errors --no-remote-name -o archive "$url"',
    'curl --retry 3 --retry-all-errors -O --no-remote-name "$url"',
    # an unused `-o -` is not a transfer
    'curl --retry 3 --retry-all-errors -o archive -o - "$url"',
    # `--` ends the options and is not a URL
    'curl --retry 3 --retry-all-errors -o archive -- "$url"',
    # a redirection is not a second transfer
    'curl -fL --retry 3 --retry-all-errors -o f "$url" 2>/dev/null',
    'curl -fL --retry 3 --retry-all-errors -o f "$url" > /dev/null 2>&1',
    'curl -fL --retry 3 --retry-all-errors -o f "$url" 2>&1 | tee log',
    "curl.exe --fail --retry 3 --retry-all-errors --output f $url 2>$null",
    'curl --retry 3 --retry-all-errors --remote-name-all "$a" "$b"',
    'curl -fL --retry 3 --retry-all-errors --max-time 120 -H "Accept: */*" -o f "$url"',
    # a quoted `;` or `#` stays in its word
    'curl -fL -o "a;b" --retry 3 --retry-all-errors "$url"',
    "curl -fL --retry 3 --retry-all-errors -o f https://example.invalid/page#part",
    # text that says curl is not a download
    "# curl -fL -o file https://example.invalid/file",
    "echo 'curl -fL -o file \"$url\"'",
    'echo "run: curl -fL -o file $url"',
    "for tool in autoconf automake pkg-config curl zip unzip tar; do",
    "sudo dnf install -y autoconf libtool pkgconf-pkg-config curl zip unzip tar",
    "python - <<'EOF'\nprint('curl -fL -o file url')\nEOF\necho done",
    # neither -f nor -o: not a download the step acts on
    "if curl -sSL --max-time 30 https://example.invalid/ 2>/dev/null | grep -qi reflection; then",
    "curl -fL -o file https://example.invalid/file # OLO_DOWNLOAD_NO_RETRY_OK: fixture",
]
# (document, the 1-based lines that must be reported)
FILES = [
    # a literal block reports the command's own line
    ("steps:\n  - run: |\n      echo start\n      curl -fL -o f https://example.invalid/f\n", [4]),
    # a folded scalar is one line to the shell: the flags on its second line count
    ('steps:\n  - run: >-\n      curl -fL --retry 3\n      --retry-all-errors -o "$file" "$url"\n', []),
    # and `curl` alone on the first line does not hide what follows it
    ('steps:\n  - run: >\n      curl\n      -fL --retry 3 -o "$file" "$url"\n', [2]),
    ("steps:\n  - run: curl -fL -o f https://example.invalid/f\n", [2]),
    # a commented-out step is not a step, and `run` is found at any depth
    ("steps:\n  # - run: curl -fL -o f https://example.invalid/f\n  - name: none\n", []),
    ("jobs:\n  a:\n    steps:\n      - name: x\n        run: |\n          Invoke-WebRequest -Uri $u -OutFile m.7z\n", [6]),
    # the line of a command that follows a here-document
    ("steps:\n  - run: |\n      cat <<EOF\n      curl -fL -o in-the-body url\n      EOF\n      curl -fL -o f url\n", [6]),
]


def self_check() -> list[str]:
    problems = []
    for script, expected in SCRIPTS_MUST_FAIL:
        got = [message for _, message in script_findings(script)]
        if got != [expected]:
            problems.append(f"self-check: expected {expected!r}, got {got}: {script!r}")
    for script in SCRIPTS_MUST_PASS:
        got = [message for _, message in script_findings(script)]
        if got:
            problems.append(f"self-check: wrongly caught ({got}): {script!r}")
    for document, expected_lines in FILES:
        lines = [line for line, _ in file_findings(document)]
        if lines != expected_lines:
            problems.append(f"self-check: lines {lines}, expected {expected_lines}: {document!r}")
    return problems


def tracked_ci_files() -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", "-z", ".github"], check=True, capture_output=True
    ).stdout.decode("utf-8", errors="replace")
    return [path for path in out.split("\0") if path.endswith((".yml", ".yaml"))]


def main() -> int:
    problems = self_check()
    if problems:
        print("\n".join(problems))
        return 1
    files = tracked_ci_files()
    count = 0
    for path in files:
        try:
            with open(path, encoding="utf-8", errors="replace") as handle:
                text = handle.read()
        except OSError:
            continue  # deleted in the working tree, still in the index
        try:
            found = file_findings(text)
        except yaml.YAMLError as error:
            found = [(getattr(getattr(error, "problem_mark", None), "line", 0) + 1, f"not readable as YAML: {error}")]
        for number, message in found:
            count += 1
            print(f"{path}:{number}: {message}")
    if count:
        print(
            f"\n{count} finding(s) in {len(files)} files: a download that does not retry every error. "
            "See docs/agent-rules/ci-downloads-retry-every-error.md."
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
