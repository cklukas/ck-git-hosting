---
description: "A Mermaid flowchart example rendered by ckdocs: nodes, shapes, edges, and subgraphs."
---
# Flowchart

[All diagram examples](../mermaid.md#diagram-examples)


**Source**

````markdown
```mermaid
flowchart TD
    A([Start]) --> B[Collect input]
    B --> C{Valid?}
    C -->|yes| D[Process]
    C -->|no| E[Report error]
    D --> F([Done])
    E --> B
```
````

**Diagram**

```mermaid
flowchart TD
    A([Start]) --> B[Collect input]
    B --> C{Valid?}
    C -->|yes| D[Process]
    C -->|no| E[Report error]
    D --> F([Done])
    E --> B
```
