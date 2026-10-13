{
  description = "Reelcut: pure-QML Basecamp view over the reelcut_core module";

  inputs = {
    # Same builder as the core (Basecamp 0.3.x).
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    # A GitHub ref, not path:../core: a relative path lock breaks a clean clone on some Nix versions.
    # The view only needs the core's interface (LIDL), so this pins the commit that defines it.
    reelcut_core.url = "github:vpavlin/reelcut-basecamp/87310f23c4433fc4e0ad6c6fde5f51b825d15691?dir=core";
    reelcut_core.inputs.logos-module-builder.follows = "logos-module-builder";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
