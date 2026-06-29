{
  description = "Logos ui_qml module (C++ backend + QML view) — replace with your description";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/tutorial-v3";
    # forum_comms.url = "github:logos-co/forum-sample-app/feat/forum-stub?dir=forum-app-module";
    forum_app.url = "path:../forum-app-module"; 
  };

  outputs = inputs@{ logos-module-builder, forum_app, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
