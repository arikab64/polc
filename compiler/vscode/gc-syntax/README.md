# gc-syntax — VSCode syntax highlighting for the gc policy DSL

## Install

Unzip this folder into your VSCode extensions directory:

```sh
# macOS / Linux
rm -rf ~/.vscode/extensions/gc-syntax
cp -R gc-syntax ~/.vscode/extensions/

# Verify the layout
ls ~/.vscode/extensions/gc-syntax/
ls ~/.vscode/extensions/gc-syntax/syntaxes/
```

You should see:
```
~/.vscode/extensions/gc-syntax/
├── package.json
├── language-configuration.json
├── README.md
├── test.gc
└── syntaxes/
    └── gc.tmLanguage.json
```

**Fully quit VSCode** (Cmd/Ctrl+Q — not just close the window) and reopen.

Open `test.gc` — you should see colors immediately.

## Troubleshooting

### Status bar says "gc" but no colors

1. Run "Developer: Inspect Editor Tokens and Scopes" from the command palette
2. Click on `ALLOW` or `TCP` — the popup should show scopes like
   `source.gc`, `keyword.control.allow.gc`, etc.
3. If you see no `source.gc` scope at all → grammar file isn't loading.
   Check `~/.vscode/extensions/gc-syntax/syntaxes/gc.tmLanguage.json` exists.
4. If you see the scopes but no color → your theme isn't styling them.
   Switch to "Dark+ (default dark)" to confirm: Cmd/Ctrl+K Cmd/Ctrl+T.

### Extension doesn't load at all

Check the extension host log: Command Palette → "Developer: Show Logs..."
→ "Extension Host". Look for errors mentioning `gc-syntax` or `source.gc`.

### Remote / WSL / SSH

Extensions must be installed on the remote host, not your local machine.
SSH into the remote and put the folder there.
