# Releasing

CI builds the six prebuilt binaries (macOS, Linux and Windows, each on x64 and
arm64), assembles them into the npm tarball with `npm run pack:release`, and
uploads it as the `npm-package` artifact of every run. The smoke jobs install
that tarball on every platform and run the CLI and the API.

## First publish (manual)

npm trusted publishing cannot create a package, so the first version is
published by hand from the CI artifact of the commit being released:

```sh
gh run download <run-id> --repo appandflow/real-size --name npm-package --dir release
npm login
npm publish ./release/real-size-0.1.0.tgz --access public
```

The `./` matters: without it npm reads `release/...` as a GitHub repository.
`<run-id>` is the green CI run of the commit on `main` (see
`gh run list --repo appandflow/real-size --branch main --workflow CI`). Publish
the tarball CI built; do not run `npm publish` from a checkout, because a local
checkout holds only the binary for the machine it was built on.

Then configure the trusted publisher on npmjs.com, in the package's settings:
GitHub Actions, organization `appandflow`, repository `real-size`, workflow
`release.yml`, no environment.

## Later releases

`.github/workflows/release.yml` works only after the first manual publish
created the package and the trusted publisher above is configured.

1. Bump `version` in `package.json` and merge it to `main`.
2. Tag the merge commit `v<version>` and push the tag.

The workflow runs CI on the tag, checks that the tag matches the package
version, and publishes the CI-built tarball with provenance through npm
trusted publishing. It needs no npm token.
