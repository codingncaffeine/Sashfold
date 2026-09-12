# Blocklists

Content blocking is data, not code. Sashfold reads the lists in this folder
(or the one named by `--blocklists <dir>`) when it starts, and asks them
before any request leaves the machine. No lookup is ever sent anywhere.

- `filters/*.txt` — filter lists in the Adblock Plus syntax EasyList is
  written in: network rules (`||host^`, `|http://…`, `*`, `^`, `$script`,
  `$third-party`, `$domain=…`, `@@` exceptions, `$important`) and the
  element-hiding rules (`##selector`, `site.example##selector`,
  `~site.example##selector`, `#@#` exceptions), which hide what they name
  through a stylesheet added to the page. The procedural (`#?#`), style
  (`#$#`), snippet (`#%#`) and scriptlet (`+js`) forms are read past. Drop
  EasyList here to use it.
- `nefarious/*.txt` — sites to keep away from: phishing and malware lists,
  as a hosts file (`0.0.0.0 host`), one host or URL per line, or the same
  Adblock syntax. A site these name is refused as a page too, with no way
  through; the page names the list and the line.

Lists are read once at start, in name order; edit a file and restart.
Nothing ships here: the lists are yours.
