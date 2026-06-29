# Considerations

## Loading and state
Modules are currently loaded as singletons, and thus have one state for all modules that use them.

## Security
Since different apps that depend on the same module share state, this must be considered for the security of an application.
Eg, a forum application with a local store and editable msgs may be able to modify a past msg to show the user something malicious.

Modules should be designed to only have state pertaining to it's execution, not to its applications.
