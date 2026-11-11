{
  description = "Reelcut: pure-QML Basecamp view over the reelcut_core module";

  inputs = {
    # Same builder as the core (Basecamp 0.3.x).
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    # A GitHub ref, not path:../core: a relative path lock breaks a clean clone on some Nix versions.
    # The view only needs the core's interface (LIDL), so this pins the commit that defines it.
    reelcut_core.url = "github:vpavlin/reelcut-basecamp/f6b579b38703e1c7f860f5680b2f556c60707102?dir=core";
    reelcut_core.inputs.logos-module-builder.follows = "logos-module-builder";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosQmlModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
