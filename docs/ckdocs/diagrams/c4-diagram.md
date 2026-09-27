# C4 diagram

[All diagram examples](../mermaid.md#diagram-examples)


**Source**

````markdown
```mermaid
C4Context
    Person(user, "Customer", "A user of the site")
    System(web, "Web App", "Serves pages")
    System(pay, "Payments", "Stripe")
    Rel(user, web, "uses")
    Rel(web, pay, "charges via")
```
````

**Diagram**

```mermaid
C4Context
    Person(user, "Customer", "A user of the site")
    System(web, "Web App", "Serves pages")
    System(pay, "Payments", "Stripe")
    Rel(user, web, "uses")
    Rel(web, pay, "charges via")
```
