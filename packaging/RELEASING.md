# Release sx130x-kiss

The tag workflow builds on Debian 12 with Rust 1.85.0. GoReleaser builds the hardware backend once per architecture and generates both Debian packages, the Homebrew cask and the Nix package recipe. The two recipes point to the same release archives and verify their checksums.

## Build a snapshot

Use an amd64 Debian Bookworm environment with Rust and rustup, GNU Make, a C compiler, `gcc-aarch64-linux-gnu`, `libc6-dev-arm64-cross`, GoReleaser OSS 2.20.2 and cargo-about 0.9.3.

```sh
git submodule update --init deps/hal deps/usp
goreleaser check
goreleaser release --snapshot --clean
```

Find the packages, tarballs and checksums under `build/dist/`. To exercise the generated cask before publication, copy `homebrew/Casks/sx130x-kiss.rb` from that directory into a local tap and point its URLs at the snapshot tarballs. Keep the generated checksums.

For Nix, place the generated `nix/sx130x-kiss.nix` alongside copies of `packaging/nix/flake.nix` and `flake.lock`. Prefetch the tarball for your architecture with `nix store prefetch-file` using its local file URL. Then install the flake directory into a test profile.

## Prepare the release channels once

From the committed main branch, publish the flake in `packaging/nix/` as the root of the `nix` branch. GoReleaser writes `sx130x-kiss.nix` alongside the flake on each release.

```sh
git subtree split --prefix=packaging/nix -b nix
git push origin nix
```

Set the Actions secret `HOMEBREW_TAP_TOKEN` on this project to a GitHub token with Contents write access limited to `guilhem/homebrew-tap`. The workflow uses its own `GITHUB_TOKEN` for this project's release and Nix branch.

## Publish a version

Make the release tag match the version in `Cargo.toml`. Commit the manifest and lockfile on main, then create and push the matching tag. For example, with version `0.1.0` in the manifest, run these commands from the release commit.

```sh
git tag v0.1.0
git push origin v0.1.0
```

Verify the tagged release assets, the cask in the tap and the package on the `nix` branch. Follow each installation path in the [user guide](../README.md#installation) and compare `sx130x-kiss --version` with the tag.
