---
description: "A Mermaid Git graph example rendered by ckdocs: commits, branches, and merges."
---
# Git graph

[All diagram examples](../mermaid.md#diagram-examples)


**Source**

````markdown
```mermaid
gitGraph
    commit
    commit tag: "v0.1"
    branch develop
    commit
    checkout main
    commit
    merge develop tag: "v1.0"
```
````

**Diagram**

```mermaid
gitGraph
    commit
    commit tag: "v0.1"
    branch develop
    commit
    checkout main
    commit
    merge develop tag: "v1.0"
```
