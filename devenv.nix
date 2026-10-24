# devenv for the Luanti fx fork. Enter with `devenv shell` (or direnv), then `fx-help`.
#
# The build is RUN_IN_PLACE: the binary lands in ./bin and reads shaders from
# ./client/shaders, so fx_shader_hot_reload picks up edits in the working tree.
# Your existing games, worlds and mods are found through LUANTI_*_PATH below.
{ pkgs, lib, config, ... }:

let
  userDir = "$HOME/.minetest"; # Luanti still uses ~/.minetest on Linux
in
{
  packages = with pkgs; [
    # toolchain
    gcc
    cmake
    ninja
    ccache
    pkg-config
    gdb
    clang-tools # clangd, clang-format

    # Luanti dependencies (mirrors upstream shell.nix)
    zlib
    zstd
    libjpeg
    libpng
    libGL
    luajit
    sdl3
    openal
    curl
    libvorbis
    libogg
    gettext
    gmp
    freetype
    sqlite
    jsoncpp

    # fx tooling
    renderdoc # frame captures (opengl3 driver)
    glslang # glslangValidator
    imagemagick # screenshots in util/fx/*.sh
    grim # screenshots on Wayland
    xorg.xorgserver # Xvfb for the headless smoke test
    mesa # llvmpipe for the headless smoke test
    (python3.withPackages (ps: [ ps.pillow ps.numpy ])) # jsl2mcl
  ];

  env = {
    CMAKE_GENERATOR = "Ninja";
    CCACHE_DIR = "${config.devenv.root}/.devenv/ccache";
    # NixOS: GL/Vulkan drivers (NVIDIA included) live here, not in the store
    LD_LIBRARY_PATH = "/run/opengl-driver/lib";
  };

  enterShell = ''
    export LUANTI_GAME_PATH="${userDir}/games"
    export LUANTI_WORLD_PATH="${userDir}/worlds"
    export LUANTI_MOD_PATH="${userDir}/mods"
    echo "Luanti fx fork. Run fx-help for commands."
  '';

  scripts = {
    fx-help.exec = ''
      cat <<'EOF'
      fx-configure        cmake configure (RelWithDebInfo, RUN_IN_PLACE, ccache)
      fx-build            incremental build (configures on first run)
      fx-run [args]       build, then run with util/fx/dev.conf (hot reload on).
                          Settings you change in-game persist in .devenv/luanti.conf.
                          FX_TEXTURE_PACK=/path/to/pack fx-run   uses a texture pack.
      fx-play <world>     fx-run straight into a world from ~/.minetest/worlds
      fx-upstream [args]  same as fx-run but with fx_enable = false, for A/B
      fx-test [args]      test harness, headless (Xvfb + llvmpipe): every case must
                          render without errors, pass its probes and match its
                          llvmpipe golden. Report: shots/test/report.html.
                          --only 'regex', --update-goldens, list
      fx-test-gpu [args]  the same on your display and GPU (4090 goldens)
      fx-unit             C++ unit tests (TestFx)
      fx-mcl <path>       link a Mineclonia checkout as games/mineclonia (mcl_* cases)
      fx-renderdoc        run under RenderDoc for a frame capture
      fx-jsl-test         run the jsl2mcl fx step tests (needs JSL2MCL_DIR)
      fx-install-mods     symlink util/fx/mods/* (fx_ambience particles) into
                          ~/.minetest/mods; then enable them per world
      EOF
    '';

    fx-configure.exec = ''
      cd "$DEVENV_ROOT"
      cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DRUN_IN_PLACE=TRUE \
        -DUSE_SDL3=ON \
        -DBUILD_SERVER=FALSE \
        -DBUILD_UNITTESTS=TRUE \
        -DENABLE_GETTEXT=FALSE \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        "$@"
      ln -sf build/compile_commands.json compile_commands.json
    '';

    fx-build.exec = ''
      cd "$DEVENV_ROOT"
      [ -f build/build.ninja ] || fx-configure
      ninja -C build "$@"
    '';

    fx-run.exec = ''
      set -e
      cd "$DEVENV_ROOT"
      fx-build
      conf=.devenv/luanti.conf
      [ -f "$conf" ] || cp util/fx/dev.conf "$conf"
      if [ -n "''${FX_TEXTURE_PACK:-}" ]; then
        sed -i '/^texture_path =/d' "$conf"
        echo "texture_path = $FX_TEXTURE_PACK" >> "$conf"
      fi
      exec bin/luanti --config "$conf" "$@"
    '';

    fx-play.exec = ''
      [ -n "''${1:-}" ] || { echo "usage: fx-play <world name in ~/.minetest/worlds>"; exit 1; }
      world="$LUANTI_WORLD_PATH/$1"; shift
      [ -d "$world" ] || { echo "no world at $world"; exit 1; }
      fx-run --go --world "$world" "$@"
    '';

    fx-upstream.exec = ''
      set -e
      cd "$DEVENV_ROOT"
      fx-build
      conf=.devenv/luanti-upstream.conf
      sed '/^fx_/d' util/fx/dev.conf > "$conf"
      echo "fx_enable = false" >> "$conf"
      exec bin/luanti --config "$conf" "$@"
    '';

    fx-test.exec = ''
      set -e
      cd "$DEVENV_ROOT"
      fx-build
      # llvmpipe from the store, not your GPU driver
      LD_LIBRARY_PATH="${pkgs.mesa}/lib:${pkgs.libGL}/lib" \
        LIBGL_DRIVERS_PATH="${pkgs.mesa}/lib/dri" \
        __GLX_VENDOR_LIBRARY_NAME=mesa \
        python3 util/fx/test/run.py "$@"
    '';

    fx-test-gpu.exec = ''
      set -e
      cd "$DEVENV_ROOT"
      fx-build
      python3 util/fx/test/run.py --gpu "$@"
    '';

    fx-unit.exec = ''
      set -e
      cd "$DEVENV_ROOT"
      fx-build
      bin/luanti --run-unittests --test-module TestFx
    '';

    fx-mcl.exec = ''
      [ -n "''${1:-}" ] || { echo "usage: fx-mcl <path to a Mineclonia checkout>"; exit 1; }
      ln -sfn "$(realpath "$1")" "$DEVENV_ROOT/games/mineclonia"
      echo "games/mineclonia -> $(realpath "$1")"
    '';

    fx-renderdoc.exec = ''
      cd "$DEVENV_ROOT"
      fx-build
      exec renderdoccmd capture -w bin/luanti --config util/fx/dev.conf "$@"
    '';

    fx-jsl-test.exec = ''
      [ -n "''${JSL2MCL_DIR:-}" ] || { echo "set JSL2MCL_DIR to your jsl2mcl checkout"; exit 1; }
      python "$JSL2MCL_DIR/tests/test_fx.py" "$DEVENV_ROOT/games/devtest"
    '';

    fx-install-mods.exec = ''
      mkdir -p "$LUANTI_MOD_PATH"
      for m in "$DEVENV_ROOT"/util/fx/mods/*/; do
        name=$(basename "$m")
        ln -sfn "$m" "$LUANTI_MOD_PATH/$name"
        echo "linked $name -> $LUANTI_MOD_PATH/$name (enable it in the world's mod list)"
      done
    '';
  };

  # `devenv test`: unit tests + the headless harness
  enterTest = ''
    fx-unit
    fx-test
  '';
}
