# embr language support (VS Code / TextMate)

`syntaxes/embr.tmLanguage.json` is a TextMate grammar in json form. vs code reads it directly.
other tools that take textmate grammars may want it too (GitHub's linguist, JetBrains' TextMate bundle support),
but sublime text does not read the json form, it has its own file in `../sublime/embr.sublime-syntax`

18 reserved words, `#` line comments and non-nesting `#[ ... ]#` block comments,
double-quoted strings whose only valid escapes are `\n \t \r \0 \" \\` 
anything else is highlighted as illegal, matching the lexer error,
ints and floats,the `->` return arrow, `...`, compound assignment,
and type annotations (`x: int`, `-> num`, `local auto x`).
`true`/`false`/`nil` are not keywords in embr but are coloured as constants anyway.

## try it (VS Code)

```bash
# from the repo root, symlink the folder into your extensions directory, then reload the window
ln -s "$PWD/tools/editor/vscode" ~/.vscode/extensions/embr-language
```

or `npx @vscode/vsce package` in this directory and install the resulting `.vsix`.

## test

`tools/editor/check_grammar.py` loads the grammar, checks every pattern compiles,
and tokenizes every `.embr` file in the repo plus the keyword list from the reference to make sure nothing drifts
(it is deliberately a regex smoke test, not a full TextMate engine)
