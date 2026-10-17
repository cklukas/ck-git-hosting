---
description: "A Mermaid quadrant chart example rendered by ckdocs: points placed in four labelled quadrants."
---
# Quadrant chart

[All diagram examples](../mermaid.md#diagram-examples)


Point labels automatically try eight nearby positions to reduce collisions with
other labels, markers, quadrant headings, and dividing lines. Coordinates, text,
and font sizes stay unchanged. The search is deterministic and bounded; dense
clusters and very long labels can still overlap. A larger chart or shorter
labels can help when there is insufficient room.


**Source**

````markdown
```mermaid
quadrantChart
    title Reach vs Engagement
    x-axis Low Reach --> High Reach
    y-axis Low Engagement --> High Engagement
    quadrant-1 Expand
    quadrant-2 Promote
    quadrant-3 Re-evaluate
    quadrant-4 Improve
    Campaign A: [0.3, 0.6]
    Campaign B: [0.7, 0.8]
```
````

**Diagram**

```mermaid
quadrantChart
    title Reach vs Engagement
    x-axis Low Reach --> High Reach
    y-axis Low Engagement --> High Engagement
    quadrant-1 Expand
    quadrant-2 Promote
    quadrant-3 Re-evaluate
    quadrant-4 Improve
    Campaign A: [0.3, 0.6]
    Campaign B: [0.7, 0.8]
```

Quadrant headings can move between inset edge positions within their own
quadrant when they cover points. Marker visibility takes priority over separating
text labels; no points are moved to make room.
