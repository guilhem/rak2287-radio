{
  description = "sx130x-kiss releases for Linux";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = { nixpkgs, ... }: {
    packages = nixpkgs.lib.genAttrs [ "x86_64-linux" "aarch64-linux" ] (system:
      let
        sx130x-kiss = nixpkgs.legacyPackages.${system}.callPackage ./sx130x-kiss.nix { };
      in {
        inherit sx130x-kiss;
        default = sx130x-kiss;
      });
  };
}
