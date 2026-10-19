// super-linter v6's TypeScript rules, restated in the flat config that
// super-linter v8 and later read: the TypeScript parser and no rules, so
// VALIDATE_TYPESCRIPT_ES checks that each *.ts parses. Without this file,
// v8's default config adds the recommended rule sets.
import tsParser from "@typescript-eslint/parser";

export default [
  {
    files: ["**/*.ts"],
    languageOptions: { parser: tsParser, sourceType: "module" },
  },
];
