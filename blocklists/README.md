# Blocklists

Content blocking is data, not code. Sashfold reads the lists in this folder
(or the one named by `--blocklists <dir>`) when it starts, and asks them
before any request leaves the machine. No lookup is ever sent anywhere.

- `filters/*.txt` — filter lists in the Adblock Plus syntax EasyList is
  written in: network rules (`||host^`, `|http://…`, `*`, `^`, `$script`,
  `$third-party`, `$domain=…`, `@@` exceptions, `$important`). Cosmetic
  rules (`##`) are read past for now. Drop EasyList here to use it.
- `nefarious/*.txt` — sites to keep away from: phishing and malware lists,
  as a hosts file (`0.0.0.0 host`), one host or URL per line, or the same
  Adblock syntax. A site these name is refused as a page too, with no way
  through; the page names the list and the line.

Lists are read once at start, in name order; edit a file and restart.
Nothing ships here: the lists are yours.
