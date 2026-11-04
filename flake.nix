{
  description = "clockr — development environment and CI toolchain";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      # shell.nix stays the single definition of the toolchain; the flake pins
      # nixpkgs for CI and for anyone who wants a reproducible local shell.
      devShells = forAll (pkgs: {
        default = import ./shell.nix { inherit pkgs; };

        # CI adds Node (wasm tests, debugger web tests, website build) and the
        # CA bundle/git needed by npm and CMake FetchContent.
        ci = import ./shell.nix {
          inherit pkgs;
          extraPackages = with pkgs; [ nodejs_22 git cacert ];
        };
      });
    };
}
