# State diagram

[All diagram examples](../mermaid.md#diagram-examples)


**Source**

````markdown
```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> Running : start
    Running --> Idle : stop
    Running --> [*] : shutdown
```
````

**Diagram**

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> Running : start
    Running --> Idle : stop
    Running --> [*] : shutdown
```
