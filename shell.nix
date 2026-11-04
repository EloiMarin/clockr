{ pkgs ? import <nixpkgs> {}, extraPackages ? [] }:

pkgs.mkShell {
  name = "clockr";

  hardeningDisable = [ "fortify" ];

  packages = (with pkgs; [
    clang
    clang-tools
    lldb

    cmake
    ninja
    pkg-config
    emscripten
  ]) ++ extraPackages;
}
