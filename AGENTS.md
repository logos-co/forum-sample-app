
# Idea to Logos App (NOT web frameworks)

See [helper-mds/module-considerations.md](helper-mds/module-considerations.md#starting-with-an-idea)

For networking, use logos-delivery (see skills below).
For storage, use logos-storage from logos-co github.
For blockchain, use logos-blockchain and logos-execution-zone repos from logos-blockchain github account.

# ref-repos
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
- [surface-metadata-in-ui](skills/surface-metadata-in-ui/SKILL.md) — display a metadata.json value (e.g. version) in the QML UI via CMake → compile-def → backend PROP → QML binding.

## IDE clangd errors
- [Verify module builds](verify-module-builds.md) — ./ build with `nix build`; IDE clangd errors there are false positives
