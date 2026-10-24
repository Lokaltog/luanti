{
  # Luanti "fx" fork: mood renderer. Day-to-day development uses devenv.nix
  # (`devenv shell`, then `fx-help`); this flake builds a package and offers a
  # minimal dev shell.
  description = "Luanti with the fx mood renderer (colored light, emissive, HDR bloom)";

  # pinned (nixos-unstable as of 2026-10-24); `nix flake lock` records the hash
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/02c087f3fe673c9e317f206a627cb6b910336dbe";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAll (pkgs: rec {
        # Reuse nixpkgs' recipe (deps, wrapping) and swap in this tree.
        # nixpkgs pins a release; its patches target that release, so drop them.
        luanti-fx = pkgs.luanti.overrideAttrs (old: {
          pname = "luanti-fx";
          version = "fx-${self.shortRev or "dirty"}";
          src = self;
          patches = [ ];
          cmakeFlags = (old.cmakeFlags or [ ]) ++ [
            "-DBUILD_SERVER=FALSE"
            "-DBUILD_UNITTESTS=FALSE"
          ];
        });
        default = luanti-fx;
      });

      apps = forAll (pkgs: rec {
        # NixOS: the GPU driver comes from /run/opengl-driver; nixpkgs' luanti
        # wrapper (via libglvnd) finds it, no LD_LIBRARY_PATH needed
        client = {
          type = "app";
          program = "${self.packages.${pkgs.stdenv.hostPlatform.system}.luanti-fx}/bin/luanti";
        };
        default = client;
      });

      # `nix develop`, then:
      #   cmake -B build -G Ninja -DRUN_IN_PLACE=TRUE -DBUILD_SERVER=FALSE
      #   ninja -C build && bin/luanti
      # RUN_IN_PLACE reads shaders from ./client/shaders, so fx_shader_hot_reload
      # picks up edits in the working tree.
      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.luanti-fx ];
          packages = with pkgs; [
            cmake
            ninja
            ccache
            clang-tools
            gdb
            renderdoc
            glslang # glslangValidator for linting shaders outside the game
            xorg.xorgserver # Xvfb for util/fx/test/run.py
            mesa # llvmpipe for util/fx/test/run.py
            (python3.withPackages (ps: [ ps.pillow ps.numpy ])) # util/fx/test/run.py
          ];
          CMAKE_GENERATOR = "Ninja";
        };
      });
    };
}
