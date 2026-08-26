---
name: mtl-ste-writing
description: 'Rewrite prose (docs, READMEs, PR descriptions, error messages, release notes, comments — never code) into ASD-STE100 Simplified Technical English. Use when asked to make writing docs, make docs clear or plain, enforce a controlled writing style, or write technical documentation that reads human. Two modes — strict (procedures/safety) and STE-flavored (general prose).'
---

# ste-writing

Write prose in ASD-STE100 Simplified Technical English. This applies to documentation, READMEs, pull-request text, error messages, release notes, and comments. It does not apply to code, identifiers, or command syntax. It is not for marketing copy, essays, or anything that needs a voice — STE strips voice on purpose.

## Rules

Cite a rule by its tag, such as [S-LEN]. Do not cite a line number, because line numbers move.

WORDS
- **[W-ONE-NAME]** Use one name for one thing. Do not call the same item by two different names.
- **[W-SHORT-WORD]** Use the short common word:
  - start (not begin/commence/initiate)
  - use (not utilize/leverage)
  - help (not facilitate)
  - make sure (not ensure)
  - before (not prior to)
  - after (not subsequent to)
  - about (not regarding/concerning)
  - get (not obtain/acquire)
  - show (not demonstrate)
  - also (not additionally/furthermore/moreover)
- **[W-ONE-MEANING]** Give each word one meaning. "fall" means to move down, not to decrease.
- **[W-NO-HYPE]** No marketing adjectives: seamless, robust, powerful, cutting-edge, effortless, world-class, next-generation, revolutionary.
- **[W-US-SPELLING]** American spelling.

VERBS
- **[V-ACTIVE]** Active voice. "the parser reads the file", not "the file is read by the parser".
- **[V-VERB-NOT-NOUN]** Use a verb for an action. "analyze the log", not "perform an analysis of the log".
- **[V-NO-AUX-STACK]** No stacked auxiliaries. Not "it is important to note that this may help to improve". Write "this improves X".
- **[V-NO-ING]** No "-ing" main verb where a simple tense works.
- **[V-NO-PHRASAL]** No phrasal verb where one verb works. Write "start the daemon", not "spin up the daemon".

SENTENCES
- **[S-ONE-IDEA]** One instruction per sentence.
- **[S-LEN]** Cap an instruction at 20 words and descriptive text at 25 words. An instruction tells the reader to act, and every step of a numbered list is an instruction. Descriptive text is everything else. No other rule here states a sentence length, and none overrides this one.
- **[S-COUNT]** Count the whitespace-separated tokens. Backticks do not fuse tokens, so `ethtool -i` counts as two. A token keeps every dot, hyphen, underscore, and slash, so `fw.app` and `E800-series` each count as one. Do not count a token that is punctuation only. Do not count the rule tag that starts a bullet.
- **[S-SCOPE]** [S-LEN] counts sentences of prose. A term list, a table row, a heading, and a command on its own line are not sentences.
- **[S-NO-CONTRACTION]** No contractions. Use articles: a, an, the, this, these.

PUNCTUATION
- **[P-NO-SEMICOLON]** No semicolons. Write two sentences. STE bans the semicolon and does not ban the em dash. If you want the em dash gone, add a rule for it yourself.

STRUCTURE
- **[T-STRUCTURE]** One topic per paragraph, max six sentences. For steps, use a numbered vertical list, one action per item, imperative form. Put a condition before its command.

Write only the requested text. No preamble, no summary, no closing remarks.

## Modes

- **strict** — procedures, runbooks, safety text, error messages: apply every rule with no exception.
- **STE-flavored** — general prose (READMEs, PR descriptions, docs): apply the SENTENCES rules, [T-STRUCTURE], [V-ACTIVE], and [V-NO-PHRASAL]. Relax the ~900-word STE dictionary, so the text keeps enough range to read naturally.

## Self-lint (run before returning text)

1. Any sentence over its [S-LEN] cap? Split it. Count the words under [S-COUNT].
2. Any semicolon? Replace it with a period. See [P-NO-SEMICOLON].
3. Any contraction? Expand it. See [S-NO-CONTRACTION].
4. Any passive voice with a known actor? Make it active. See [V-ACTIVE].
5. Any "-ing" main verb or nominalization ("perform an analysis")? Replace it with a plain verb. See [V-NO-ING] and [V-VERB-NOT-NOUN].
6. Any phrasal verb ("spin up")? Replace it with one verb. See [V-NO-PHRASAL].
7. Same thing named two ways? Pick one name. See [W-ONE-NAME].

The mechanical rules above are lintable and are what removes slop. Full STE also needs human judgment: the right technical noun, and whether a sentence makes good sense. A checker cannot certify that, and slop is not about that. This skill fixes the FORM of slop. It cannot make a hollow paragraph true.

Free official standard (do not paste it in full; it is copyrighted): <https://asd-ste100.org>
