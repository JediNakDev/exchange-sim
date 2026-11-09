# exsim research UI design system

Status: source of truth for every screen in `web/`. Adopted 2026-11-08. It replaces the harbor-blue workstation direction of 2026-11-06 (see git history of this file).

Read this before touching anything under `web/src`. If a screen needs something this document does not cover, extend the document in the same change.

## 1. Subject, audience, job

- **Subject.** exsim, a deterministic exchange simulator. People configure an experiment, run it across a fixed set of seeds, compare two experiments, then inspect individual fills and events.
- **Audience.** Execution and market-making researchers. They read numbers for a living, distrust anything that looks rounded or decorative, and keep the app open next to code and notebooks.
- **Primary job.** Make exact results easy to read and easy to audit. Every screen answers "what did the strategy do, and can I trust that number?"

## 2. Direction: the exact ledger

A light, flat, square workspace that reads like a well-kept trading ledger. Green marks the one thing you can act on and the values that went in your favour. Everything else is ink on paper.

Rejected directions, and why:

- **Dark terminal with neon green.** The default look for anything trading and green. It also hurts long reading sessions of dense tables.
- **SaaS card kit** (every block in a rounded, shadowed card). It is what the previous UI drifted into: cards inside sections inside cards, with no hierarchy.
- **Broadsheet hairlines everywhere.** Too editorial for a tool people operate.

The one memorable element is the **signed figure**: every signed number (P&L, markouts, differences) is set in tabular Geist Mono with only its sign coloured, green when it favours the strategy and red when it does not, while the digits stay in ink. Numeric columns are right aligned with fixed decimal places, so a column of results scans like a ledger margin of coloured signs. Boldness is spent there and nowhere else.

## 3. Foundation

### Stack

- shadcn/ui preset `b6snPS1nE` (`bunx --bun shadcn@latest init --preset b6snPS1nE`). In this existing Radix project it resolves to style `radix-lyra`, base colour `olive`, icon library Lucide. Lyra means square corners, compact 12 px controls and flat surfaces.
- Tailwind CSS v4 utilities only. No bespoke CSS classes for layout or colour. `globals.css` holds tokens and base rules, nothing else.
- Lucide icons only (`lucide-react`). Default size comes from the component (16 px, 14 px in `sm` controls). Stroke width stays at the Lucide default.
- Add shadcn components with `bunx --bun shadcn@latest add <name>`. Never fork a primitive to restyle it; pass `className` at the call site. The only local change to a generated primitive is `CardTitle` rendering an `h2`, so card titles stay headings for screen readers.

### Colour tokens

All colour comes from CSS variables in `web/src/app/globals.css`. Never write a hex value or a Tailwind palette colour (`text-emerald-600`, `bg-slate-50`) in a component.

| Token | Light | Dark | Role |
| --- | --- | --- | --- |
| `primary` | `#007a55` | `#00bc7d` | The action you take next. Primary button, active navigation, focus ring, selected row marker. Dark mode uses a brighter green with dark text so it still passes as text. |
| `foreground` | `#0c0c09` | `#fbfbf9` | Ink. All body text and every number. |
| `muted-foreground` | `#6a6a57` | `#abab9c` | Labels, descriptions, units, table headers. Darkened from the preset so 12 px text passes WCAG AA (5.0:1 on `muted`). |
| `background` / `card` | `#ffffff` | `#0c0c09` / `#1d1d16` | Page and plotting surfaces. |
| `muted` / `sidebar` | `#f4f4f0` / `#fbfbf9` | `#2b2b22` / `#1d1d16` | Recessed surfaces: table headers, toolbars, code, the sidebar. |
| `border` | `#e8e8e3` | white 10% | Every rule and outline. |
| `positive` | `#00774b` | `#30cb92` | Sign of a favourable signed figure. Never a background. |
| `negative` / `destructive` | `#cc2827` | `#f66c6d` | Sign of an unfavourable figure, destructive actions, failures. |
| `warning` | `#9d6300` | `#e9b452` | Paused, stale, censored or approximate data. Always with an icon or word. |

Colour is never the only signal. State badges carry a word, signed figures carry a `+` or `-` character, chart series carry a legend and a label.

### Chart palette

Validated with the dataviz palette checker (lightness band, chroma floor, colour-blind separation, contrast) in both modes.

| Slot | Light | Dark | Use |
| --- | --- | --- | --- |
| `chart-1` | `#00875f` | `#1fae7e` | Candidate, or the only series |
| `chart-2` | `#5b5bd6` | `#8a86ec` | Baseline |
| `chart-3` | `#c2710c` | `#c97a22` | Third series |
| `chart-4` | `#2f7bb8` | `#4b9be0` | Fourth series |
| `chart-5` | `#b04a7a` | `#c95f8e` | Fifth series |

Rules: assign slots in this fixed order and never cycle. Baseline is always `chart-2` and candidate always `chart-1` on every compare chart. Lines are 1.5 px, no dots except on hover. Grid lines use `border`, axis text uses `muted-foreground` at 10 px. One y-axis per chart. Every chart keeps its "Read sampled values" table.

### Typography

Two families, clearly distinct, both from the preset and loaded with `next/font` (self-hosted at build time).

- **Inter** (`font-sans`) for all interface text.
- **Geist Mono** (`font-mono`) for data only: prices, quantities, cash, IDs, digests, nanosecond times, seeds. Never for labels or headings.
- Every number, mono or not, uses `tabular-nums`.

Scale (ratio about 1.2, anchored on lyra's 12 px control size):

| Step | Class | Use |
| --- | --- | --- |
| 24 px | `text-2xl font-semibold tracking-tight` | Page title (`h1`), one per page |
| 18 px | `text-lg font-mono` | Key figures in a `KeyFigureList` |
| 16 px | `text-base font-medium` | Section title (`h2`) |
| 14 px | `text-sm` | Prose: page description, empty states, explanations |
| 12 px | `text-xs` | Controls, tables, labels, card content (the lyra default) |
| 10 px | `text-[10px]` | Chart axes only |

Weights: 400 body, 500 titles and emphasis, 600 the page title only. Sentence case everywhere. No all-caps labels, no eyebrow text above headings, no italic or coloured single words inside headings. Prose lines stay under 80 characters (`max-w-prose`).

### Space, shape, depth

- 4 px grid. Page gutter `px-6` (`px-4` under 640 px). Vertical rhythm: `gap-8` between sections, `gap-3` inside a section, `gap-2` between controls.
- Square corners everywhere (`rounded-none` from lyra). The only round things are status dots and avatars.
- Flat. No shadows on surfaces. Depth comes from `border` and from `muted` recessed areas. Popovers and dialogs keep the primitive's own shadow. The one exception is the selection marker: a 2 px inset `primary` edge (`shadow-[inset_2px_0_0_var(--color-primary)]`) on selected table rows (built into `TableRow`) and on the active sidebar item. It marks state, it does not add depth.

### Motion

- No entrance animations, no hover lifts.
- Motion only answers an action: collapsible open, dialog open, toast. The single ambient motion is the pulsing dot on a `running` run, because that state is literally changing. All motion stops under `prefers-reduced-motion`.

## 4. Layout

```text
+----------------+---------------------------------------------------------------+
| exsim          | [=]  Research / Run analysis                        [theme]  |  48 px header
|                +---------------------------------------------------------------+
| Research     < |  Seed 101  (o completed)                    [Pause] [Inspect]|  PageHeading
| Strategies     |  run_x / exp_y, deterministic research                         |
| Scenarios      |                                                               |
|                |  Net P&L     Realized     Ending pos.   Max drawdown           |  KeyFigureList
|                |  +78.80000   +71.0000     -100          15.80000               |
|                |  -----------------------------------------------------------  |
|                |  Section title                                    [action]    |  ResearchSection
| ...            |  Section description                                          |
| New study      |  [ table / chart / form ]                                     |
| Synthetic,     |                                                               |
| single venue   |                                                               |
+----------------+---------------------------------------------------------------+
```

- **Shell.** shadcn `Sidebar` (`collapsible="icon"`, becomes a sheet on mobile) plus `SidebarInset`. The header holds `SidebarTrigger`, a `Breadcrumb` for the current location and the theme menu. No footer.
- **Sidebar.** Brand mark; Research, Strategies, Scenarios, Runs and Compare as primary destinations; New study as the creation action. Connections is a read-only utility in SidebarFooter alongside the research-mode note. Research groups persisted variant lineage into studies; exact experiment/run identities remain unchanged. Existing experiment URLs remain deep links. This navigation amendment implements the user-requested workflow revamp; all visual tokens and primitive rules above remain authoritative.
- **Content.** Left aligned, `max-w-screen-2xl`, never centred text except inside an empty state.
- **Page.** `PageHeading`, optional `KeyFigureList`, then a stack of `ResearchSection`s. Two-column grids (`xl:grid-cols-2`) only for genuinely parallel content (exchange truth versus strategy view, ledger versus orders).
- **Cards** are for self-contained objects that make sense alone: a chart, the launch panel, a fill explanation. A section that is just a heading and a table is not a card.
- **Tables** sit in a bordered container (`border bg-card`), header row on `muted` with `muted-foreground` text (built into `TableHeader` and `TableHead`), 12 px text, numeric columns right aligned in mono. Every table has an accessible name (`aria-label` or caption). Clickable rows show the focus ring when reached by keyboard. Wide tables scroll inside their container, never the page.
- **Responsive.** Works at 390 px wide with no horizontal page scroll. Key figures wrap to two columns, page actions wrap under the title.

## 5. Shared research components

These live in `web/src/components/research/` and are the only custom presentational components. Build screens from them plus shadcn primitives.

| Component | What it does |
| --- | --- |
| `PageHeading` | `h1`, optional description, actions on the right (wrap under on narrow screens). |
| `ResearchSection` | A `section` with an `h2`, optional description and actions, then content. Labelled by its heading. |
| `KeyFigureList` | A `dl` of 2 to 5 headline figures in a ruled strip. Label in `muted-foreground`, value in 18 px mono. |
| `DefinitionList` | Two-column `dl` for label and value facts. Values in mono when they are data. |
| `SignedFigure` | Renders an already formatted exact decimal string with a coloured sign. `favourableDirection` is `higher` (default), `lower` (costs, drawdown, shortfall) or `neutral` (positions, plain differences). Never parses the number, and its text content equals the input string. |
| `RunStateBadge` | Outline badge with a state dot and the state word. Colours: running `primary` (pulsing), completed `positive`, paused `warning`, failed `negative`, queued and cancelled `muted-foreground`. |
| `ApiFailureAlert` | Destructive `Alert` with error code, message, field issues, request id and a "Try again" button. |

## 6. Component usage rules

- **Buttons.** One `default` (green) button per view region, for the next step. Secondary actions use `outline`, tertiary `ghost`, links that look like text use `link`. Destructive actions use `destructive`. Size `sm` in toolbars and tables, `default` elsewhere. Icons go before the label with `data-icon="inline-start"`. No arrows appended to button text.
- **Badges.** `outline` for states and categories, `secondary` for neutral metadata such as "exact" or "derived". Never use `default` (solid green) badges; solid green is reserved for the primary action.
- **Forms.** shadcn `Field`, `FieldSet`, `FieldLegend`, `FieldLabel`, `FieldDescription`, `FieldError`. Units appear in the label in `muted-foreground`. Optional groups collapse with `Collapsible`, not native `details`.
- **Search.** `InputGroup` with a leading `SearchIcon` addon.
- **Empty states.** shadcn `Empty` with a Lucide icon, a title that names the situation and one action that resolves it.
- **Loading.** `Skeleton` shaped like the content it replaces. Inline progress uses `Spinner`.
- **Keyboard hints.** `Kbd`.
- **Disclosure of long evidence** (sampled values, raw manifests): `Collapsible` with a ghost trigger button.
- **Toasts.** `sonner`, past-tense message that names the action ("Run paused").

## 7. Writing

- Name things by what the researcher does: "Compare selected", "Inspect events", "Create and start 5 runs".
- An action keeps the same verb through the flow: a "Pause" button produces a "Run paused" toast.
- Errors say what failed and how to recover. They never apologise.
- Lists in metadata are joined with commas, not middle dots.
- Keep every existing accessible name, role and label text unless the change is deliberate and its tests change with it. Browser tests address the UI by role and name.

## 8. Engineering rules (from Vercel React best practices)

- Import directly from module paths, never from barrel files (`bundle-barrel-imports`).
- Do not define components inside components (`rerender-no-inline-components`). Hoist static JSX and constant arrays to module scope (`rendering-hoist-jsx`).
- Use ternaries, not `&&`, for conditional rendering of values that could be `0` or `""` (`rendering-conditional-render`).
- Derive state during render, not in effects (`rerender-derived-state-no-effect`).
- Keep `"use client"` at the leaves. Layout and static shell stay server components where the primitive allows.
- Heavy optional panels (charts) may load with `next/dynamic` when they are below the fold.
- Exact decimals stay strings. Only `src/lib/chart-derived-floats.ts` may turn them into numbers (enforced by ESLint).

## 9. Accessibility floor

- Visible focus on every interactive element (`ring` token). Skip link to `#research-content`.
- Text contrast at least 4.5:1, large text and chart marks at least 3:1, in both themes.
- Every table has an accessible name. Every chart has a text alternative (the sampled values table).
- Light and dark themes follow the OS setting by default, with a manual override in the header.

## 10. Review checklist for any UI change

1. No hex values, no Tailwind palette colours, no new CSS classes in components.
2. Uses shared research components and shadcn primitives; no hand-rolled equivalents.
3. One page title, sections with `h2`, sentence case, no eyebrows.
4. Numbers in mono and tabular, signed values through `SignedFigure`.
5. Works at 390 px and in dark mode.
6. `bun run typecheck`, `bun run lint`, `bun test` and the browser tests pass.

## 11. Research workspace hierarchy, 2026-11-08

Research is the landing destination. It groups baseline configurations and variants by persisted parent lineage, exposes the question/description and attention states, and links to controlled comparisons. Strategy and scenario libraries use actual service capabilities and presets. Runs is the cross-study attempt queue; completed counts are not eligibility counts. Configuration pages have Results, Runs and recovery, and Assumptions sections. Native comparison remains the authority for cohorts, reproduction and censoring.

Run analysis leads with strategy-specific outcomes. Execution Overview shows completion/cost and a bounded decision-based progress plot with explicit coverage and delivered-knowledge semantics; exact trace and child evidence live in Explain execution. MM Overview uses time-aligned equity/inventory/drawdown samples and coverage-aware per-horizon markout bars. Diagnostics, state history and provenance/export remain secondary sections. Equity includes initial capital and is never relabeled net P&L. Comparison leads with paired trade-offs, then contextual assumptions/eligibility and paths. Research findings export to local evidence-bound files with explicit persistence wording. No metadata save is fabricated.

Performance: fetch independent resources concurrently, load heavy optional chart panels on demand, keep exact decimals in strings and derive approximate plot positions only through chart-derived-floats. Do not read full event streams for library rows. This implementation does not introduce native parameter sweeps, uploaded strategies or a second metadata store.


## Configuration and navigation amendment, 2026-11-09

Follow both `frontend-design` and `vercel-react-best-practices` within the token,
typography and primitive rules above. The active configuration workflow uses the four built-in research strategies.

- Built-in research: task, explicit strategy selector, strategy parameters,
  market assumptions and run plan, review, launch, paired comparison, inspection.
  The strategy selector belongs immediately below the task choice. A direct
  parameter anchor focuses the relevant group. Research executes the strategy and exchange in separate workers; the UI
  does not select a research transport protocol.
- Custom strategy setup, protocol selection, handoff export and Coming soon
  entries are removed. Connections remains read-only telemetry. Missing research
  API telemetry does not establish whether an independently started native
  server is running.
- Breadcrumb ancestors are actual Next links; only the current page is a
  noninteractive `BreadcrumbPage`. Inspection includes Runs and its Run analysis
  ancestor, preserving baseline, candidate and pinned run selection. Variants
  link to their source study. Breadcrumbs stay on one locally scrolling line
  on narrow screens so the fixed header and document do not overflow.

Configuration flow artifact:
[CONFIGURATION_FLOW_2026-11-09.html](reviews/CONFIGURATION_FLOW_2026-11-09.html).
