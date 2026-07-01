---
name: use-design-system
description: Restyle a Logos ui_qml module's QML view with the Logos design system — swap raw QtQuick.Controls + hardcoded hex colors for Logos.Controls components and Theme.palette/spacing/typography tokens. Covers the import path (no flake.nix change), the control swaps, the color/spacing/typography token mapping, and the LogosTextField API differences. Use when a module's Main.qml is styled with bespoke colors and you want it to match the design system.
---

# Use the Logos design system in a QML view

The design system ships two QML modules — `Logos.Theme` (design tokens:
`Theme.palette`, `Theme.spacing`, `Theme.typography`) and `Logos.Controls`
(themed components: `LogosText`, `LogosButton`, `LogosTextField`, …). Adopting
it means **replacing raw `QtQuick.Controls` + hardcoded hex** with these.

Reference: [repos/logos-design-system/README.md](../../repos/logos-design-system/README.md)
(token catalog + control list). The canonical worked example is
[repos/logos-delivery-demo/src/qml/Main.qml](../../repos/logos-delivery-demo/src/qml/Main.qml);
[src/qml/Main.qml](../../src/qml/Main.qml) in this repo is a second one. Browse
live tokens/controls with `nix run` in the design-system repo (the storybook).

## No flake.nix change needed

The `Logos.*` modules are **not** declared by the app. They're on the QML import
path provided by the host that runs the module (`logos-standalone-app` →
`logos-design-system`, pulled transitively via `logos-module-builder`). So a
consumer just imports them:

```qml
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Theme
import Logos.Controls
```

> `nix build .` compiles/packages the C++ plugin but does **not** resolve QML
> imports — those bind at runtime in the host. To actually see the restyle,
> launch via the standalone host app / Basecamp, not `nix build`.

## Paint the background

The host window is transparent underneath, so the view must paint its own
surface. Add a fill rect as the first child of the root `Item`:

```qml
Rectangle {
    anchors.fill: parent
    color: Theme.palette.background
}
```

## Swap the controls

| Raw | Design system | Notes |
|---|---|---|
| `Text` | `LogosText` | `Text` subclass — `font.*`, `wrapMode`, `elide`, `color` all still work |
| `TextField` | `LogosTextField` | wraps a `TextInput`; see API differences below |
| `Button` | `LogosButton` | `text`, `onClicked`, `enabled`, `radius`; default 200×50 — override `implicitWidth/Height` for compact buttons |
| `ComboBox` | `LogosComboBox` | `model`, `currentIndex`, `currentText` |

Full designed set: `LogosButton`, `LogosCheckbox`, `LogosComboBox`,
`LogosIconButton`, `LogosPaginator`, `LogosSearchBar`, `LogosTabBar`,
`LogosTabButton`, `LogosTable`, `LogosText`, `LogosTextField` (+ placeholders like
`LogosBadge`, `LogosDialog`, `LogosSpinner`, …). See the README for the split.

## `LogosTextField` API differences (the common snag)

It's a `Control` wrapping an inner `TextInput`, not a `TextField`. So:

```qml
LogosTextField { id: titleField; placeholderText: "…"; enabled: root.nodeReady }

// Enter/Return: NO onAccepted on the control — handle it on the inner input.
Connections {
    target: titleField.textInput
    function onAccepted() { bodyField.textInput.forceActiveFocus() }
}
```

- **Text**: `titleField.text` (read/write alias) works. There is **no `clear()`** —
  use `titleField.text = ""`.
- **Focus**: focus the inner input — `titleField.textInput.forceActiveFocus()`,
  not `titleField.forceActiveFocus()`.
- **Inner input**: `textInput` is a read-only alias for advanced use
  (`cursorPosition`, `select`, `onAccepted`, …).

`Connections` is a non-visual `QtObject`, so it can sit inside a `RowLayout` /
`ColumnLayout` next to the field without affecting layout.

## Replace hardcoded values with tokens

Never hardcode hex, px spacing, or font sizes. Map to semantic tokens:

### Colors → `Theme.palette.*`

| Was (GitHub-dark) | Token | Role |
|---|---|---|
| `#ffffff`, `#e6edf3` | `text` | primary text |
| `#c9d1d9`, `#8b949e` | `textSecondary` | body / muted |
| `#6e7681` | `textTertiary` | dim captions, placeholders-in-empty-state |
| view backdrop | `background` | root fill |
| dark inset list panel `#0d1117` | `backgroundInset` | recessed list container |
| card `#161b22` | `backgroundSecondary` | raised card on inset |
| row `#0d1117` panel (delivery-demo) | `backgroundElevated` | inner rows |
| border `#30363d` | `borderHairline` | subtle 1px borders |
| accent / selected border `#1f6feb` | `primary` | accent (orange in dark theme) |
| translucent accent fill `#1f6feb33` | `overlayOrange` | selected-row fill |
| `#56d364` | `success` · `#f0883e` → `warning` · `#f85149` → `error` | status |

Full semantic set (surfaces, text, borders, primary, status, interactive,
overlays): see the README "Tokens at a glance".

### Spacing / radii → `Theme.spacing.*`

`tiny:4`, `small:8`, `medium:12`, `large:16`, `xlarge:20`, `xxlarge:40`;
`radiusSmall:4`, `radiusMedium:6`, `radiusLarge:8`, `radiusXlarge:16`,
`radiusPill:999`. Use for `anchors.margins`, `spacing`, `radius`, `padding`.

### Type → `Theme.typography.*`

Family `publicSans`; weights `weightRegular:400` / `weightMedium:500` /
`weightBold:700`; sizes `badgeText:8`, `secondaryText:12`, `primaryText:14`,
`subtitleText:16`, `panelTitleText:24`, `titleText:30`, `pageTitleText:36`,
`mainTitleText:256`. Use `font.pixelSize` + `font.weight` (not `font.bold`).

```qml
LogosText {
    text: "Broadcast Forum"
    font.pixelSize: Theme.typography.panelTitleText
    font.weight: Theme.typography.weightBold
    color: Theme.palette.text
}
```

### Monospace (code-like values)

There is **no mono token**. For peer ids / hashes / topics / timestamps,
centralise a generic family on the root and set `font.family`:

```qml
readonly property string monoFont: "monospace"   // Qt maps to platform fixed-pitch
// … font.family: root.monoFont
```

## Rectangle borders

Setting `border.color` alone renders a 1px border, but the design-system code
sets `border.width: 1` explicitly alongside `border.color` — match that.

## Gotchas

- **`LogosTextField` ≠ `TextField`** — no `onAccepted`/`clear()` on the control;
  go through `.textInput` (and set `.text = ""` to clear). Focus the inner input.
- **Paint your own background** — the host is transparent; without the fill rect
  the view renders on whatever's behind it.
- **No flake.nix input** for the design system — it arrives via the host. Adding
  it as a flake input is unnecessary and not the established pattern.
- **`nix build` won't catch QML import typos** — QML binds at runtime. Verify by
  running in the host, and copy component/token names from the storybook or an
  existing worked view rather than guessing.
- **Semantic over raw** — reference `Theme.palette.surfaceRaised`, never a raw
  `Theme.colors.grayNNN`; the semantic layer is the consumer-facing contract.
