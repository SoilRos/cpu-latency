{
  description = "Measure the CPU latency between cores";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f:
        nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "cpu-latency";
          version = "0.1.0";
          src = ./.;

          nativeBuildInputs = [ pkgs.cmake pkgs.pkg-config ];
          buildInputs = [ pkgs.hwloc ];

          installPhase = ''
            runHook preInstall
            install -Dm755 cpu-latency $out/bin/cpu-latency
            install -Dm755 ../cpu-latency-plot $out/bin/cpu-latency-plot
            runHook postInstall
          '';
        };
      });

      apps = forAllSystems (pkgs:
        let
          system = pkgs.stdenv.hostPlatform.system;
          pythonEnv = pkgs.python3.withPackages (ps: [ ps.matplotlib ps.numpy ]);
          cpu-latency-plot = pkgs.writeShellScriptBin "cpu-latency-plot" ''
            exec ${pythonEnv}/bin/python3 ${self.packages.${system}.default}/bin/cpu-latency-plot "$@"
          '';
        in
        {
          default = self.apps.${system}.cpu-latency;
          cpu-latency = {
            type = "app";
            program = "${self.packages.${system}.default}/bin/cpu-latency";
          };
          cpu-latency-plot = {
            type = "app";
            program = "${cpu-latency-plot}/bin/cpu-latency-plot";
          };
        });
    };
}
