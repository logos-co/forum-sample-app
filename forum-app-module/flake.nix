{
  description = "Calculator module - wraps libcalc C library for Logos";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/tutorial-v3";
    # forum_comms.url = "github:logos-co/forum-sample-app/feat/forum-stub?dir=forum-comms";
    forum_comms.url = "path:../forum-comms"; 
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
      tests = {
        dir = ./tests;
      };
    };
}