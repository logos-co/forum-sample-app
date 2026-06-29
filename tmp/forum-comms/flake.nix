{
  description = "Forum Comms Module - Manages communication of forum data and updates";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/tutorial-v3";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
