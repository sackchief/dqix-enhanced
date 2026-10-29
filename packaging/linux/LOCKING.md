# Linux x86_64 Conda lock

- Direct dependency policy: `environment.yml`.
- File consumed by CI/AppImage packaging: `conda-linux-64.lock`.
- Format: micromamba explicit single-platform package list with exact package builds and URL MD5 fragments.
- Platform: `linux-64`; URLs must target `linux-64` or `noarch`.
- Explicit package count: 96.
- Lock SHA-256: `5aeec2066f1d9fcfbf729de3ea6531559c8b9b0a31065a610059fb18793bae96`.
- Source spec SHA-256 at generation: `8f0701ca304014c0c49c0d6e5ce8f577843caf22ff50a1a45bf250aabb55b866`.
- Captured from the successful CI environment in run `40730463353`, commit `dbf93e0d287eaba5b0c42d45f66ec0d9c30c311e`.

## Regeneration

On Linux x86_64, create the environment from `environment.yml`, then run:

```sh
micromamba env export --name dqix-builder --explicit > conda-linux-64.lock
sha256sum conda-linux-64.lock
```

Review URL/build changes; update `conda-linux-64.lock.sha256` and `conda-lock-meta.json` together. CI installs from this explicit lock and checks the installed package URL set against it. Changing only `environment.yml` deliberately fails the lock metadata test until the lock is regenerated and reviewed.
