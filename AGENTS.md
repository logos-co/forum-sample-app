
## ref-repos

Refers to manully checked out repos for AI agents to refer to, .gitignor'ed since they are not part of the app.

If a skill refers to a logos repo, check ref-repos. If not present, consider cloning it. eg `cd ref-repos && git clone git@github.com:logos-co/logos-tutorial.git`

# Modules
Information about module glue and loading [here](helper-mds/module-actions.md), and broader lifecycle information [here](helper-mds/module-info.md).


## Skills
Reusable procedures live in `skills/`. Read the relevant one before the matching task:
- [rename-logos-module](skills/rename-logos-module/SKILL.md) — rename a Logos C++ module end to end.
- [use-another-module](skills/use-another-module/SKILL.md) — make one module call/subscribe to another via the generated `modules().<dep>` wrapper.
- [use-delivery-module](skills/use-delivery-module/SKILL.md) — send/receive messages over the Logos network using `delivery_module`.
- [use-design-system](skills/use-design-system/SKILL.md) — use the design system (compatible with basecamp) in a ui module of an app
