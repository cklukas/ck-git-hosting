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
