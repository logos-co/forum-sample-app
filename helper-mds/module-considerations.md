# Considerations

## Starting with an idea

### App

For an idea that is like an app, start with a UI module & cpp backend - https://github.com/logos-co/logos-tutorial/blob/tutorial-v4/outputs/tutorial-cpp-ui-app.md, this uses [logos-module-builder](https://github.com/logos-co/logos-module-builder/) template `#ui-qml-backend`. (`tutorial-v4` is the latest tag at the time of writing).

Apps should use the [logos-design-system](https://github.com/logos-co/logos-design-system), unless there are heavily bespoke UI design requirements.

### Reusable primitive

For an idea that is about bringing capabilities to other modules, eg a cryptographic primitive:
 - If a library already exists, it can be wrapped. See the template `#with-external-lib` in logos-module-builder.
 - If it is some c, consider the default template in logos-module-builder
 - If it is some rust code, consider [logos-rust-sdk](https://github.com/logos-co/logos-rust-sdk) and the [logos-rust-example-module](https://github.com/logos-co/logos-rust-example-module)

### Composing Modules

In certain scenarios, the technical design might warrant a library that serves as a helper to use another module, separating private state. 

When a module needs to call another module, info [here](https://github.com/logos-co/logos-tutorial/blob/tutorial-v4/outputs/tutorial-composing-modules.md), or if decoupling via an interface info [here](https://github.com/logos-co/logos-tutorial/blob/tutorial-v4/outputs/tutorial-interface-dependencies.md).

## Loading and state
Modules are currently loaded as singletons, and thus have one state for all modules that use them.

## Security
Since different apps that depend on the same module share state, this must be considered for the security of an application.
Eg, a forum application with a local store and editable msgs may be able to modify a past msg to show the user something malicious.

Modules should be designed to only have state pertaining to it's execution, not to its applications.
