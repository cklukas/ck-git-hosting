# Class diagram

[All diagram examples](../mermaid.md#diagram-examples)


**Source**

````markdown
```mermaid
classDiagram
    Animal <|-- Dog
    Animal <|-- Cat
    Animal : +String name
    Animal : +int age
    Animal : +makeSound() void
    Dog : +fetch() void
```
````

**Diagram**

```mermaid
classDiagram
    Animal <|-- Dog
    Animal <|-- Cat
    Animal : +String name
    Animal : +int age
    Animal : +makeSound() void
    Dog : +fetch() void
```
