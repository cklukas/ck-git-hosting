# Gantt chart

[All diagram examples](../mermaid.md#diagram-examples)


**Source**

````markdown
```mermaid
gantt
    title Project plan
    dateFormat YYYY-MM-DD
    section Design
    Research :a1, 2024-01-01, 7d
    Mockups :a2, after a1, 5d
    section Build
    Backend :2024-01-10, 10d
    Frontend :after a2, 8d
```
````

**Diagram**

```mermaid
gantt
    title Project plan
    dateFormat YYYY-MM-DD
    section Design
    Research :a1, 2024-01-01, 7d
    Mockups :a2, after a1, 5d
    section Build
    Backend :2024-01-10, 10d
    Frontend :after a2, 8d
```
