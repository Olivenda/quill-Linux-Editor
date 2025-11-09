# Contributing to Quill

Thank you for your interest in improving Quill! With your help, our editor becomes better for everyone. The following guidelines are meant to help you get started. They are recommendations, not strict rules.

---

## Good Bug Reports

When reporting a bug, please:

1. **Don’t open duplicates:** First, search to see if your issue already exists. Duplicate issues will be closed.
2. **Provide complete information:**  
   - **Summary:** A short description of the issue.
   - **Steps to reproduce:** Write detailed steps on how to recreate the bug.
   - **Expected behavior:** Describe what should happen.
   - **Actual behavior:** Say what happens instead, including any error message or crash output.
   - **System details:** Quill version, OS, installation method.

Missing info often leads to clarification requests. Unanswered issues may be closed.

---

## Branch Naming Conventions (Required!)

For every change, **use an appropriate branch name**. This helps us immediately identify the purpose of your work:

- **feature/<short-description>** — For new features in the editor.  
  _Branch:_ `feature/`
- **fix/<short-description>** — For bug and crash fixes.  
  _Branch:_ `fix/`
- **refactor/<short-description>** — For code restructuring, cleanup or improving architecture.  
  _Branch:_ `refactor/`
- **docs/<short-description>** — For documentation changes only.  
  _Branch:_ `docs/`
- **test/<short-description>** — For new or updated test code.  
  _Branch:_ `test/`

**When to use which branch?**  
- Adding new features → `feature/`
- Fixing bugs/crashes → `fix/`
- Improving structure, reorganizing code → `refactor/`
- Documentation improvements only → `docs/`
- Adding or changing tests → `test/`

**Commits made in the wrong branch may be rejected!**  
Following these naming conventions is mandatory—our workflow and review process depends on it.

---

## Pull Requests

- Keep your PR focused and limited to one topic.
- Write your C code in the style already present (formatting, indentation, brackets).
- Maximum line length is 125 characters.
- Commit messages must be in present tense ("Fix memory leak", not "Fixed memory leak").
- First line ≤ 72 characters; add references to issues (e.g. `#123`) after the first line.

Minor formatting issues may be fixed on rebase, but please try to follow the standards to keep our workflow clean.

---

## One Final Note

Quill is a C-based editor, inspired by classic terminal editors. These conventions are in place to make collaboration easier and to simplify fixing bugs and building features.

Thank you for contributing to Quill!