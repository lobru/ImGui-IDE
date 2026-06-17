---
description: Render an interactive inline checklist widget from a list of items
argument-hint: <item; item; item>  (or paste a numbered/bulleted list)
---

Render an interactive checklist as an inline widget using the `mcp__visualize__show_widget` tool (the Imagine suite). This is the reusable template for the editor feedback-loop checklist style.

Items to render: $ARGUMENTS

Rules:
- Parse `$ARGUMENTS` into discrete items, split on `;`, newlines, or leading `-`/`*`/`N.` bullets. If empty, ask the user for the list (one short sentence) and stop.
- Each item: detect an optional leading priority tag (`P1`/`P2`/`P3`/`OK`/`done`) and an optional ` - subtitle` / `: subtitle`. Default priority `P2`, no subtitle.
- Build the widget with the SAME look as the project's roadmap checklist:
  - One row per item: a toggle check box (left), a colored priority pill, title + muted subtitle, and a `Start ↗` button (omit the button for `OK`/`done` rows, render them checked + struck-through).
  - Priority pill colors via host CSS vars: OK→success, P1→danger, P2→warning, P3→info.
  - Persist checkbox state in `localStorage` under a key derived from the item set (e.g. `checklist_<hash>`); show an `N / total` progress count.
  - `Start ↗` calls `sendPrompt('Let's do this next: "<title>" (<subtitle>). Scope it, then start. Terse.')`.
  - A footer `Reset` button clears the saved checks.
- Follow all Imagine design rules: no titles/prose inside the widget (put any explanation in your reply text), dark-mode-safe colors via CSS variables, Tabler outline icons only, sentence case.
- Keep your surrounding chat reply to 1-2 lines — the widget carries the detail.

Reference implementation: `roadmap-checklist.html` at the repo root (standalone twin of this widget).
