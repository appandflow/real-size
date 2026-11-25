# Releasing

CI builds the six prebuilt binaries (macOS, Linux and Windows, each on x64 and
arm64), assembles them into the npm tarball with `npm run pack:release`, and
uploads it as the `npm-package` artifact of every run. The smoke jobs install
that tarball on every platform and run the CLI and the API.

## First publish (done)

0.1.0 was published by hand from the `npm-package` artifact of CI run
45666109276 (commit `ba0d021`), because npm trusted publishing cannot create a
package. npm staged that first publish: for about a minute the registry showed
a stub version `0.0.0-stage` before 0.1.0 appeared, and the stub stays listed
in the package's versions.

The repository has no `v0.1.0` tag: the release workflow at `ba0d021` cannot
skip a version that already exists, so a tag there would run a failing
publish. Commit `ba0d021` is the 0.1.0 source.

The trusted publisher is configured on npmjs.com (package Settings, Trusted
Publisher), so the next version is the first tag-driven release:

| Field                | Value          |
| -------------------- | -------------- |
| Publisher            | GitHub Actions |
| Organization or user | `appandflow`   |
| Repository           | `real-size`    |
| Workflow filename    | `release.yml`  |
| Environment name     | (empty)        |

To publish by hand again, from the CI artifact of the commit being released:

```sh
gh run download <run-id> --repo appandflow/real-size --name npm-package --dir release
npm login
npm publish ./release/real-size-<version>.tgz --access public
```

The `./` matters: without it npm reads `release/...` as a GitHub repository.
Publish the tarball CI built; a local checkout holds only the binary for the
machine it was built on.

## Tag-driven releases

1. Bump `version` in `package.json` and merge it to `main`.
2. Tag the merge commit `v<version>` and push the tag.

The workflow checks that the tag matches the package version and that the
version is not on npm yet; when it already is, as 0.1.0 is, it stops there
with a notice and succeeds. Otherwise it runs CI on the tag and publishes the
CI-built tarball with provenance through npm trusted publishing. It needs no
npm token.
