---
description: "A Mermaid entity-relationship diagram example rendered by ckdocs: entities and their cardinalities."
---
# Entity-relationship

[All diagram examples](../mermaid.md#diagram-examples)


**Source**

````markdown
```mermaid
erDiagram
    CUSTOMER ||--o{ ORDER : places
    ORDER ||--|{ LINE_ITEM : contains
    CUSTOMER {
        string name
        string email
    }
```
````

**Diagram**

```mermaid
erDiagram
    CUSTOMER ||--o{ ORDER : places
    ORDER ||--|{ LINE_ITEM : contains
    CUSTOMER {
        string name
        string email
    }
```
