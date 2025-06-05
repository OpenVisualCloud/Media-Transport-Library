# Coding standard guideline

## Commit messages
For Mtl commit messages generally follows Conventional Commit messages rules
For more details on the Conventional Commit messages rules, refer to the
[Conventional Commits specification](https://www.conventionalcommits.org/en/v1.0.0/).

The only deviations are that structural elements (like Add or Fix) should be capitalized,
and we also allow `add` as an replacement for feat and `build(deps)` for CiCd dependencies bumps.

- `Build`: Changes that affect the build tool or external dependencies (example scopes: gulp, broccoli, - npm)
- `Ci`: Changes to our CI configuration files and scripts (example scopes: Travis, Circle, BrowserStack, - Sauce Labs)
- `Docs`: Documentation only changes
- `Feat` / `Add`: A new feature
- `Fix`: A bugfix
- `Perf`: A code change that improves performance
- `Refactor`: A code change that neither fixes a bug nor adds a feature
- `Style`: Changes that do not affect the meaning of the code (whitespace, formatting, missing - semi-colons, etc)
- `Test`: Adding missing tests or correcting existing tests


## Coding standard

The coding standard for the Media Transport Library project is defined using a [.clang-format configuration file](../.github/linters/.clang-format)
which ensures consistent code formatting across the project.

Use `format-code.sh` script to format the code for mtl repository ( requires `clang-format-14` ).
```bash
./format-code.sh
```
