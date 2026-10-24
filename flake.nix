{
  description = "E-Ink shim";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    rust-overlay = {
      url = "github:oxalica/rust-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs =
    { nixpkgs, rust-overlay, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];
      forAll =
        f:
        nixpkgs.lib.genAttrs systems (
          system:
          f (
            import nixpkgs {
              inherit system;
              overlays = [ rust-overlay.overlays.default ];
            }
          )
        );
    in
    {
      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          packages = [
            # build/soong rust/config/global.go RustDefaultVersion on lineage-24.0
            (pkgs.rust-bin.stable."1.93.1".minimal.override {
              extensions = [
                "clippy"
                "rustfmt"
              ];
              targets = [ "aarch64-linux-android" ];
            })
          ];
        };
      });
    };
}
