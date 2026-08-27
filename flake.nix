{
  description = "Example Forum — Logos ui_qml module (C++ backend + QML view)";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    # Core module dependency — must match metadata.json "dependencies".
    # Pinned to v0.2.1: v0.1.x publishes no LIDL contract, which current
    # logos-module-builder requires to generate the consumer wrapper.
    delivery_module.url = "github:logos-co/logos-delivery-module/v0.2.1";
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
