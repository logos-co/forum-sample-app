{
  description = "Example Forum — Logos ui_qml module (C++ backend + QML view)";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # Core module dependency — must match metadata.json "dependencies".
    # Pinned to v0.1.3 (the version this app's delivery usage targets).
    delivery_module.url = "github:logos-co/logos-delivery-module/v0.1.3";
    # Core module dependency — must match metadata.json "dependencies".
    # Using jzaki/keystore-signer-module for per-caller key isolation.
    keystore_signer.url = "github:jzaki/keystore-signer-module";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
