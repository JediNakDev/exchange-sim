# Research workstation design

Applied skill: Anthropic `frontend-design`, installed from `anthropics/skills/skills/frontend-design` on 2026-11-06. The installed SKILL.md was read before design work. This is a design change to the existing Bun/Next.js/TypeScript/Tailwind/shadcn application, not a backend or API redesign.

## Direction

A calm workstation for comparing controlled market experiments and explaining fills. The memorable element is a deep harbor-blue research rail beside a light analysis canvas. Charts and dense records carry the page; there is no marketing hero, faux live market ticker, decorative gradient or neon trading-terminal treatment.

| Token | Value | Role |
| --- | --- | --- |
| Harbor | `#17394e` | Persistent navigation and identity |
| Canvas | `#eef3f7` | Workspace background |
| Paper | `#ffffff` | Reading and plotting surfaces |
| Ink | `#203849` | Primary text |
| Teal | `#157c80` | Reference trace and primary actions |
| Bronze | `#ba7339` | Candidate trace and changed assumptions |

IBM Plex Sans is the self-hosted interface/display face. IBM Plex Mono is reserved for amounts, IDs and timestamps. Main titles use a 30 px scale; body text is 14 px, with compact 12 px metadata. Sentence case and left alignment throughout. Color is paired with text, line labels and state names.

## Layout plan and critique

Considered a full-width terminal with a dark plotting canvas. Rejected: it privileges a familiar trading aesthetic over the actual job of reading assumptions and comparing evidence. The revised direction keeps navigation dark but gives charts, forms and records a quiet light surface.

```text
Research rail      Workspace context                       Research mode
                   Page title / description                Primary action
Experiments        Real experiment counts / run outcomes
Compare            Main evidence: studies, charts or event tape
Connections        Supporting assumptions / ledger / lifecycle
```

On narrow screens the rail becomes a compact top navigation. Tables retain their own horizontal scrolling rather than stretching the page. The event tape and numeric records remain compact, with visible keyboard focus and reduced-motion support. No extra animation is needed to make a research result legible.

All counts and outcome values must come from the existing API. Existing exact decimal strings, error states, controls, idempotency, comparison provenance and exchange/strategy visibility distinctions are preserved. Shared shadcn primitives remain the control foundation. Typography is self-hosted, with no runtime font request.

## Build critique and verification

The first built form crowded long scenario labels into three columns. The final form uses a wider editor and two-column groups, with readable domain labels and their original units. Technical parameter keys and exact string values remain unchanged in requests. Small secondary text was darkened for legibility on the cool canvas. The event toolbar stays available while reading lifecycle evidence; exchange truth and delivered strategy data retain distinct labels.

The real production pages were exercised at 1440 px and 390 px: library search and its empty result, experiment/run views, preview validation, comparison, keyboard event stepping and fill selection. All four mobile page checks retained a 390 px document width. Browser exceptions and failed API responses: zero. Unit/contract frontend tests: 59 passed; typecheck, lint and standalone production build passed. The refreshed walkthrough also creates a real paused run, steps it and resumes it to completion. C++ behavior and the HTTP contract are unchanged.

A final failure-state check intercepts an unavailable experiment service: the three activity counts show `n/a`, not false zeroes. This case failed against the initial redesign and passes in the deployed version.
