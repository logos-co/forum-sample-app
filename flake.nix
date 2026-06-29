{
  description = "Broadcast App — Logos ui_qml module (C++ backend + QML view)";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # Core module dependency — must match metadata.json "dependencies".
    # Pinned to v0.1.3 (the version this app's delivery usage targets).
    delivery_module.url = "github:logos-co/logos-delivery-module/v0.1.3";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
