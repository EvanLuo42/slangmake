# slangmake docs

Topic-focused references. The top-level [README](../README.md) is the quickstart / CLI / blob-format reference; the docs here go deeper on the pieces an engine author actually has to think about.

- **[cursor.md](cursor.md)** — `ReflectionView::Cursor`, the `ShaderCursor`-style navigator. Field and array navigation, auto-dereference, `ParameterBlock` descent, descriptor-set layout enumeration, an end-to-end Vulkan binding example, and what the Cursor *can't* do versus Slang's live-session equivalent.

- **[permutations.md](permutations.md)** — the permutation model: constant axes (`// [permutation] NAME={a,b,c}`) and type axes (`// [permutation type] NAME={T1,T2}`). How the file directives, CLI overrides and Cartesian expander interact, how permutations are keyed in the blob, and when to reach for which axis kind.

- **[integration.md](integration.md)** — the shipping-build integration recipe: mmap the blob, look up entries, build pipeline objects, handle device-lost recovery, relate to the graphics API's pipeline cache, and size up what you gain by not shipping `slang.dll`.

- **[reflection.md](reflection.md)** — raw-table reflection API for when the Cursor isn't enough: walking the decl tree, enumerating binding / descriptor / sub-object ranges directly, decoding user attributes, correlating variables and layouts, using the string and hashed-string pools.
