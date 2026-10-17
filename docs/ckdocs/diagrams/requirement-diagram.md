---
description: "A Mermaid requirement diagram example rendered by ckdocs: requirements and the elements that satisfy them."
---
# Requirement diagram

[All diagram examples](../mermaid.md#diagram-examples)


**Source**

````markdown
```mermaid
requirementDiagram
    requirement test_req {
        id: 1
        risk: high
    }
    element test_entity {
        type: simulation
    }
    test_entity - satisfies -> test_req
```
````

**Diagram**

```mermaid
requirementDiagram
    requirement test_req {
        id: 1
        risk: high
    }
    element test_entity {
        type: simulation
    }
    test_entity - satisfies -> test_req
```
