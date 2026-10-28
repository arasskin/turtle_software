{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  nativeBuildInputs = with pkgs; [
    valgrind
    clang-tools
    gdb
    bear
    libsodium
    liburing
  ];
}
