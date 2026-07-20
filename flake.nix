{
  description = "Example Forum — Logos ui_qml module (C++ backend + QML view)";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # Core module dependency — must match metadata.json "dependencies".
    # Pinned to v0.1.3 (the version this app's delivery usage targets).
    delivery_module.url = "github:logos-co/logos-delivery-module/v0.1.3";
    # Core module dependency — must match metadata.json "dependencies". No
    # tagged release exists yet (unlike delivery_module above), so this
    # tracks master rather than a fixed ref. Switch to a version tag once
    # logos-accounts-module cuts one.
    accounts_module.url = "github:logos-co/logos-accounts-module";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
