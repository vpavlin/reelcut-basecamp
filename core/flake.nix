{
  description = "reelcut_core: Basecamp core module that drives reelcut (long video to reels)";

  inputs = {
    # Basecamp 0.3.x: builder 0.3.1 (release tag).
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.1";
    # Logos Storage, for the reels: this computer's node (the host's), or a node elsewhere through
    # storage_gateway (the same API). Pinned: master once pulled in an API break.
    storage_module.url = "github:logos-co/logos-storage-module/v3.0.1";
    storage_gateway.url = "github:vpavlin/logos-storage-gateway/585aba6d22916895856f6830822e04e981b5f51e?dir=module";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
    };
}
