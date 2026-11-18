# Test plugins: the built-in language servers

Each folder declares one of t-code's built-in language servers through a plugin's `lsp` field, with
the same command line, options and settings. They are tests, not shipped (t-code only scans
`plugins/`, not `tests/plugins/`):

- `tests/test_plugins.c` (`/plugins/lsp-test-plugins`) loads them and checks that with the built-in
  servers turned off each file goes to its plugin's server, started the same way as the built-in one.
- To try one by hand, turn the server off in Settings → Language Servers and copy its folder into
  `~/.config/t-code/plugins/`.

pylsp's Python and rust-analyzer's Cargo features come from the project's toolchain for the built-in
servers; a plugin's options and settings are fixed, so these two leave them out.
