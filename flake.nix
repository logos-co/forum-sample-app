{
  description = "Example Forum — Logos ui_qml module (C++ backend + QML view)";

  inputs = {
    # Pinned to 0.2.6 to match cloud-data-module, whose cloud_data_core library
    # is vendored under lib/ — the two need the same codegen.
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.2.6";
    # Core module dependency — must match metadata.json "dependencies".
    # Pinned to v0.2.1: v0.1.x publishes no LIDL contract, which current
    # logos-module-builder requires to generate the consumer wrapper.
    delivery_module.url = "github:logos-co/logos-delivery-module/v0.2.1";
    delivery_module.inputs.logos-module-builder.follows = "logos-module-builder";
    # Core module dependency — must match metadata.json "dependencies".
    # Backs cloud_data_core's durability/snapshot bridge (BlobStore).
    storage_module.url = "github:logos-co/logos-storage-module/v2.1.2";
    storage_module.inputs.logos-module-builder.follows = "logos-module-builder";
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
