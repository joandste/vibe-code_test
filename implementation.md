### 13.11 Reference Implementation Notes

Not part of the spec, but relevant to the reference implementation:

- Target license: MIT.
- Development: multi-file.
- Optional build target: amalgamated `lang.c` and `lang.h` for easy embedding. (skip for now)
- Evaluator strategy: TBD (tree-walking is the natural first choice and should be used now).
- GC: TBD.
- Embedding API: TBD.
