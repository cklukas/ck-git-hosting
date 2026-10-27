---
description: "A Mermaid XY chart example rendered by ckdocs: bars and lines over shared axes, with automatic label rotation."
---
# XY chart

[All diagram examples](../mermaid.md#diagram-examples)


Crowded x-axis labels rotate automatically. The renderer reserves space for
their measured height and horizontal overhang, so ordinary first and last labels
remain within the chart. Extra margins are limited to preserve useful plot space;
very long or numerous labels may still need a larger chart or horizontal bars.
Labels are not truncated by the renderer. If a name is already shortened in the
Mermaid source, change the source generator to restore the full name.

Every `bar` and `line` statement is a series of its own, and
`xychart-beta horizontal` turns the chart on its side.
[XY charts in depth](../xy-charts.md) shows both.


**Source**

````markdown
```mermaid
xychart-beta
    title "Monthly revenue"
    x-axis [Jan, Feb, Mar, Apr]
    y-axis "Revenue (k)" 0 --> 100
    bar [30, 55, 45, 80]
    line [20, 40, 38, 70]
```
````

**Diagram**

```mermaid
xychart-beta
    title "Monthly revenue"
    x-axis [Jan, Feb, Mar, Apr]
    y-axis "Revenue (k)" 0 --> 100
    bar [30, 55, 45, 80]
    line [20, 40, 38, 70]
```
