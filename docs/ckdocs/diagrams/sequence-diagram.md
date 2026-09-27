# Sequence diagram

[All diagram examples](../mermaid.md#diagram-examples)


**Source**

````markdown
```mermaid
sequenceDiagram
    participant U as User
    participant S as Server
    participant DB as Database
    U->>S: POST /login
    S->>DB: lookup user
    DB-->>S: user record
    S-->>U: 200 OK + token
    Note over U,S: session established
```
````

**Diagram**

```mermaid
sequenceDiagram
    participant U as User
    participant S as Server
    participant DB as Database
    U->>S: POST /login
    S->>DB: lookup user
    DB-->>S: user record
    S-->>U: 200 OK + token
    Note over U,S: session established
```
