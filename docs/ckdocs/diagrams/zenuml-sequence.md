# ZenUML sequence

[All diagram examples](../mermaid.md#diagram-examples)


**Source**

````markdown
```mermaid
zenuml
    title Order flow
    User->Web: place order
    Web->API.createOrder()
    API->DB: insert
    API->Web: 201
    Web->User: confirmation
```
````

**Diagram**

```mermaid
zenuml
    title Order flow
    User->Web: place order
    Web->API.createOrder()
    API->DB: insert
    API->Web: 201
    Web->User: confirmation
```
